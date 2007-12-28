/*
 * trsolver.cpp - transient solver class implementation
 *
 * Copyright (C) 2004, 2005, 2006, 2007 Stefan Jahn <stefan@lkcc.org>
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this package; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street - Fifth Floor,
 * Boston, MA 02110-1301, USA.  
 *
 * $Id: trsolver.cpp,v 1.52 2007-12-28 20:08:47 ela Exp $
 *
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <float.h>

#if HAVE_IEEEFP_H
# include <ieeefp.h>
#endif

#include "object.h"
#include "logging.h"
#include "complex.h"
#include "circuit.h"
#include "sweep.h"
#include "net.h"
#include "analysis.h"
#include "nasolver.h"
#include "history.h"
#include "trsolver.h"
#include "transient.h"
#include "exception.h"
#include "exceptionstack.h"

#ifdef __MINGW32__
# define finite(x) _finite(x)
#endif

#define STEPDEBUG   0 // set to zero for release
#define BREAKPOINTS 0 // exact breakpoint calculation

#define dState 0 // delta T state
#define sState 1 // solution state

using namespace transient;

// Constructor creates an unnamed instance of the trsolver class.
trsolver::trsolver ()
  : nasolver<nr_double_t> (), states<nr_double_t> () {
  swp = NULL;
  type = ANALYSIS_TRANSIENT;
  setDescription ("transient");
  for (int i = 0; i < 8; i++) solution[i] = NULL;
  tHistory = NULL;
  relaxTSR = false;
  initialDC = true;
}

// Constructor creates a named instance of the trsolver class.
trsolver::trsolver (char * n)
  : nasolver<nr_double_t> (n), states<nr_double_t> () {
  swp = NULL;
  type = ANALYSIS_TRANSIENT;
  setDescription ("transient");
  for (int i = 0; i < 8; i++) solution[i] = NULL;
  tHistory = NULL;
  relaxTSR = false;
  initialDC = true;
}

// Destructor deletes the trsolver class object.
trsolver::~trsolver () {
  if (swp) delete swp;
  for (int i = 0; i < 8; i++) if (solution[i] != NULL) delete solution[i];
  if (tHistory) delete tHistory;
}

/* The copy constructor creates a new instance of the trsolver class
   based on the given trsolver object. */
trsolver::trsolver (trsolver & o)
  : nasolver<nr_double_t> (o), states<nr_double_t> (o) {
  swp = o.swp ? new sweep (*o.swp) : NULL;
  for (int i = 0; i < 8; i++) solution[i] = NULL;
  tHistory = o.tHistory ? new history (*o.tHistory) : NULL;
  relaxTSR = o.relaxTSR;
  initialDC = o.initialDC;
}

// This function creates the time sweep if necessary.
void trsolver::initSteps (void) {
  if (swp != NULL) return;
  swp = createSweep ("time");
}

// Macro for the n-th state of the solution vector history.
#define SOL(state) (solution[(int) getState (sState, (state))])


// Performs the initial DC analysis.
int trsolver::dcAnalysis (void) {
  int error = 0;

  // First calculate a initial state using the non-linear DC analysis.
  setDescription ("initial DC");
  initDC ();
  setCalculation ((calculate_func_t) &calcDC);
  solve_pre ();
  applyNodeset ();

  // Run the DC solver once.
  try_running () {
    error = solve_nonlinear ();
  }
  // Appropriate exception handling.
  catch_exception () {
  case EXCEPTION_NO_CONVERGENCE:
    pop_exception ();
    convHelper = CONV_LineSearch;
    logprint (LOG_ERROR, "WARNING: %s: %s analysis failed, using line search "
	      "fallback\n", getName (), getDescription ());
    applyNodeset ();
    restart ();
    error = solve_nonlinear ();
    break;
  default:
    // Otherwise return.
    estack.print ();
    error++;
    break;
  }

  // Save the DC solution.
  storeSolution ();

  // Cleanup nodal analysis solver.
  solve_post ();

  // Really failed to find initial DC solution?
  if (error) {
    logprint (LOG_ERROR, "ERROR: %s: %s analysis failed\n",
	      getName (), getDescription ());
  }
  return error;
}

/* This is the transient netlist solver.  It prepares the circuit list
   for each requested time and solves it then. */
int trsolver::solve (void) {
  nr_double_t time, saveCurrent;
  int error = 0, convError = 0;
  char * solver = getPropertyString ("Solver");
  relaxTSR = !strcmp (getPropertyString ("relaxTSR"), "yes") ? true : false;
  initialDC = !strcmp (getPropertyString ("initialDC"), "yes") ? true : false;

  runs++;
  saveCurrent = current = 0;
  stepDelta = -1;
  converged = 0;
  fixpoint = 0;
  statRejected = statSteps = statIterations = statConvergence = 0;

  // Create time sweep if necessary.
  initSteps ();

  // Choose a solver.
  if (!strcmp (solver, "CroutLU"))
    eqnAlgo = ALGO_LU_DECOMPOSITION;
  else if (!strcmp (solver, "DoolittleLU"))
    eqnAlgo = ALGO_LU_DECOMPOSITION_DOOLITTLE;
  else if (!strcmp (solver, "HouseholderQR"))
    eqnAlgo = ALGO_QR_DECOMPOSITION;
  else if (!strcmp (solver, "HouseholderLQ"))
    eqnAlgo = ALGO_QR_DECOMPOSITION_LS;
  else if (!strcmp (solver, "GolubSVD"))
    eqnAlgo = ALGO_SV_DECOMPOSITION;

  // Perform initial DC analysis.
  if (initialDC) {
    error = dcAnalysis ();
    if (error)
      return -1;
  }

  // Initialize transient analysis.
  setDescription ("transient");
  initTR ();
  setCalculation ((calculate_func_t) &calcTR);
  solve_pre ();
  swp->reset ();

  // Recall the DC solution.
  recallSolution ();

  // Apply the nodesets and adjust previous solutions.
  applyNodeset (false);
  fillSolution (x);

  // Tell integrators to be initialized.
  setMode (MODE_INIT);

  int running = 0;
  rejected = 0;
  delta /= 10;
  fillState (dState, delta);
  adjustOrder (1);

  // Start to sweep through time.
  for (int i = 0; i < swp->getSize (); i++) {
    time = swp->next ();
    logprogressbar (i, swp->getSize (), 40);

#if DEBUG && 0
    logprint (LOG_STATUS, "NOTIFY: %s: solving netlist for t = %e\n",
	      getName (), (double) time);
#endif

    do {
#if STEPDEBUG
      if (delta == deltaMin) {
	logprint (LOG_ERROR,
		  "WARNING: %s: minimum delta h = %.3e at t = %.3e\n",
		  getName (), (double) delta, (double) current);
      }
#endif
      updateCoefficients (delta);

      // Run predictor.
      error += predictor ();

      // restart Newton iteration
      if (rejected) {
	restart ();      // restart non-linear devices
	rejected = 0;
      }

      // Run corrector process.
      try_running () {
	error += corrector ();
      }
      // Appropriate exception handling.
      catch_exception () {
      case EXCEPTION_NO_CONVERGENCE:
	pop_exception ();

	// Reduce step-size if failed to converge.
	if (current > 0) current -= delta;
	delta /= 2;
	if (delta <= deltaMin) {
	  delta = deltaMin;
	  adjustOrder (1);
	}
	if (current > 0) current += delta;

	// Update statistics.
	statRejected++;
	statConvergence++;
	rejected++;
	converged = 0;
	error = 0;

	// Start using damped Newton-Raphson.
	convHelper = CONV_SteepestDescent;
	convError = 2;

#if DEBUG
	logprint (LOG_ERROR, "WARNING: delta rejected at t = %.3e, h = %.3e "
		  "(no convergence)\n", (double) saveCurrent, (double) delta);
#endif
	break;
      default:
	// Otherwise return.
	estack.print ();
	error++;
	break;
      }
      if (error) return -1;
      if (rejected) continue;

      // check whether Jacobian matrix is still non-singular
      if (!A->isFinite ()) {
	logprint (LOG_ERROR, "ERROR: %s: Jacobian singular at t = %.3e, "
		  "aborting %s analysis\n", getName (), (double) current,
		  getDescription ());
	return -1;
      }

      // Update statistics and no more damped Newton-Raphson.
      statIterations += iterations;
      if (--convError < 0) convHelper = 0;

      // Now advance in time or not...
      if (running > 1) {
	adjustDelta (time);
	adjustOrder ();
      }
      else {
	fillStates ();
	nextStates ();
	rejected = 0;
      }

      saveCurrent = current;
      current += delta;
      running++;
      converged++;

      // Tell integrators to be running.
      setMode (MODE_NONE);

      // Initialize or update history.
      if (running > 1) {
	updateHistory (saveCurrent);
      }
      else {
	initHistory (saveCurrent);
      }
    }
    while (saveCurrent < time); // Hit a requested time point?
    
    // Save results.
#if STEPDEBUG
    logprint (LOG_STATUS, "DEBUG: save point at t = %.3e, h = %.3e\n",
	      (double) saveCurrent, (double) delta);
#endif

#if BREAKPOINTS
    saveAllResults (saveCurrent);
#else
    saveAllResults (time);
#endif
  }
  solve_post ();
  logprogressclear (40);
  logprint (LOG_STATUS, "NOTIFY: %s: average time-step %g, %d rejections\n",
	    getName (), (double) (saveCurrent / statSteps), statRejected);
  logprint (LOG_STATUS, "NOTIFY: %s: average NR-iterations %g, "
	    "%d non-convergences\n", getName (),
	    (double) statIterations / statSteps, statConvergence);

  // cleanup
  deinitTR ();
  return 0;
}

// The function initializes the history.
void trsolver::initHistory (nr_double_t t) {
  // initialize time vector
  tHistory = new history ();
  tHistory->append (t);
  tHistory->self ();
  // initialize circuit histories
  nr_double_t age = 0.0;
  circuit * root = subnet->getRoot ();
  for (circuit * c = root; c != NULL; c = (circuit *) c->getNext ()) {
    if (c->hasHistory ()) {
      c->applyHistory (tHistory);
      saveHistory (c);
      if (c->getHistoryAge () > age) age = c->getHistoryAge ();
    }
  }
  // set maximum required age for all circuits
  tHistory->setAge (age);
}

/* The following function updates the histories for the circuits which
   requested them. */
void trsolver::updateHistory (nr_double_t t) {
  if (t > tHistory->last ()) {
    // update time vector
    tHistory->append (t);
    // update circuit histories
    circuit * root = subnet->getRoot ();
    for (circuit * c = root; c != NULL; c = (circuit *) c->getNext ()) {
      if (c->hasHistory ()) saveHistory (c);
    }
    tHistory->drop ();
  }
}

// Stores node voltages and branch currents in the given circuits history.
void trsolver::saveHistory (circuit * c) {
  int N = countNodes ();
  int r, i, s = c->getSize ();
  for (i = 0; i < s; i++) {
    // save node voltages
    r = findAssignedNode (c, i);
    if (r < 0)
      c->appendHistory (i, 0.0);
    else
      c->appendHistory (i, x->get (r));
  }
  for (i = 0; i < c->getVoltageSources (); i++) {
    // save branch currents
    r = c->getVoltageSource () + i;
    c->appendHistory (i + s, x->get (r + N));
  }
}

/* This function predicts a start value for the solution vector for
   the successive iterative corrector process. */
int trsolver::predictor (void) {
  int error = 0;
  switch (predType) {
  case INTEGRATOR_GEAR: // explicit GEAR
    predictGear ();
    break;
  case INTEGRATOR_ADAMSBASHFORD: // ADAMS-BASHFORD
    predictBashford ();
    break;
  case INTEGRATOR_EULER: // FORWARD EULER
    predictEuler ();
    break;
  default:
    *x = *SOL (1);  // This is too a simple predictor...
    break;
  }
  saveSolution ();
  *SOL (0) = *x;
  return error;
}

// Stores the given vector into all the solution vectors.
void trsolver::fillSolution (tvector<nr_double_t> * s) {
  for (int i = 0; i < 8; i++) *SOL (i) = *s;
}

/* The function predicts the successive solution vector using the
   explicit Adams-Bashford integration formula. */
void trsolver::predictBashford (void) {
  int N = countNodes ();
  int M = countVoltageSources ();
  nr_double_t xn, dd, hn;

  // go through each solution
  for (int r = 0; r < N + M; r++) {
    xn = predCoeff[0] * SOL(1)->get (r); // a0 coefficient
    for (int o = 1; o <= predOrder; o++) {
      hn = getState (dState, o);         // previous time-step
      // divided differences
      dd = (SOL(o)->get (r) - SOL(o + 1)->get (r)) / hn;
      xn += predCoeff[o] * dd;           // b0, b1, ... coefficients
    }
    x->set (r, xn);                      // save prediction
  }
}

/* The function predicts the successive solution vector using the
   explicit forward Euler integration formula.  Actually this is
   Adams-Bashford order 1. */
void trsolver::predictEuler (void) {
  int N = countNodes ();
  int M = countVoltageSources ();
  nr_double_t xn, dd, hn;

  for (int r = 0; r < N + M; r++) {
    xn = predCoeff[0] * SOL(1)->get (r);
    hn = getState (dState, 1);
    dd = (SOL(1)->get (r) - SOL(2)->get (r)) / hn;
    xn += predCoeff[1] * dd;
    x->set (r, xn);
  }
}

/* The function predicts the successive solution vector using the
   explicit Gear integration formula. */
void trsolver::predictGear (void) {
  int N = countNodes ();
  int M = countVoltageSources ();
  nr_double_t xn;

  // go through each solution
  for (int r = 0; r < N + M; r++) {
    xn = 0;
    for (int o = 0; o <= predOrder; o++) {
      // a0, a1, ... coefficients
      xn += predCoeff[o] * SOL(o + 1)->get (r);
    }
    x->set (r, xn); // save prediction
  }
}

/* The function iterates through the solutions of the integration
   process until a certain error tolerance has been reached. */
int trsolver::corrector (void) {
  int error = 0;
  error += solve_nonlinear ();
  return error;
}

// The function advances one more time-step.
void trsolver::nextStates (void) {
  circuit * root = subnet->getRoot ();
  for (circuit * c = root; c != NULL; c = (circuit *) c->getNext ())
    c->nextState ();
  *SOL (0) = *x; // save current solution
  nextState ();
  statSteps++;
}

/* This function stores the current state of each circuit into all
   other states as well.  It is useful for higher order integration
   methods in order to initialize the states after the initial
   transient solution. */
void trsolver::fillStates (void) {
  circuit * root = subnet->getRoot ();
  for (circuit * c = root; c != NULL; c = (circuit *) c->getNext ()) {
    for (int s = 0; s < c->getStates (); s++)
      c->fillState (s, c->getState (s));
  }
}

// The function modifies the circuit lists integrator mode.
void trsolver::setMode (int state) {
  circuit * root = subnet->getRoot ();
  for (circuit * c = root; c != NULL; c = (circuit *) c->getNext ())
    c->setMode (state);
}

// The function passes the time delta array to the circuit list.
void trsolver::setDelta (void) {
  circuit * root = subnet->getRoot ();
  for (circuit * c = root; c != NULL; c = (circuit *) c->getNext ())
    c->setDelta (deltas);
}

/* This function tries to adapt the current time-step according to the
   global truncation error. */
void trsolver::adjustDelta (nr_double_t t) {
  deltaOld = delta;
  delta = checkDelta ();
  if (delta > deltaMax) delta = deltaMax;
  if (delta < deltaMin) delta = deltaMin;

  // delta correction in order to hit exact breakpoint
  int good = 0;
  if (!relaxTSR) { // relaxed step raster?
    if (!statConvergence || converged > 64) { /* Is this a good guess? */
      // check next breakpoint
      if (stepDelta > 0.0) {
	// restore last valid delta
	delta = stepDelta;
	stepDelta = -1.0;
      }
      else {
	if (delta > (t - current) && t > current) {
	  // save last valid delta and set exact step
	  stepDelta = deltaOld;
	  delta = t - current;
	  good = 1;
	}
	else {
	  stepDelta = -1.0;
	}
      }
      if (delta > deltaMax) delta = deltaMax;
      if (delta < deltaMin) delta = deltaMin;
    }
  }

  // usual delta correction
  if (delta > 0.9 * deltaOld || good) { // accept current delta
    nextStates ();
    rejected = 0;
#if STEPDEBUG
    logprint (LOG_STATUS,
	      "DEBUG: delta accepted at t = %.3e, h = %.3e\n",
	      (double) current, (double) delta);
#endif
  }
  else if (deltaOld > delta) { // reject current delta
    rejected++;
    statRejected++;
#if STEPDEBUG
    logprint (LOG_STATUS,
	      "DEBUG: delta rejected at t = %.3e, h = %.3e\n",
	      (double) current, (double) delta);
#endif
    if (current > 0) current -= deltaOld;
  }
  else {
    nextStates ();
    rejected = 0;
  }
}

/* The function can be used to increase the current order of the
   integration method or to reduce it. */
void trsolver::adjustOrder (int reduce) {
  if ((corrOrder < corrMaxOrder && !rejected) || reduce) {
    if (reduce) {
      corrOrder = 1;
    } else if (!rejected) {
      corrOrder++;
    }

    // adjust type and order of corrector and predictor
    corrType = correctorType (CMethod, corrOrder);
    predType = predictorType (corrType, corrOrder, predOrder);

    // apply new corrector method and order to each circuit
    circuit * root = subnet->getRoot ();
    for (circuit * c = root; c != NULL; c = (circuit *) c->getNext ()) {
      c->setOrder (corrOrder);
      setIntegrationMethod (c, corrType);
    }
  }
}

/* Goes through the list of circuit objects and runs its calcDC()
   function. */
void trsolver::calcDC (trsolver * self) {
  circuit * root = self->getNet()->getRoot ();
  for (circuit * c = root; c != NULL; c = (circuit *) c->getNext ()) {
    c->calcDC ();
  }
}

/* Goes through the list of circuit objects and runs its calcTR()
   function. */
void trsolver::calcTR (trsolver * self) {
  circuit * root = self->getNet()->getRoot ();
  for (circuit * c = root; c != NULL; c = (circuit *) c->getNext ()) {
    c->calcTR (self->current);
  }
}

/* Goes through the list of non-linear circuit objects and runs its
   restartDC() function. */
void trsolver::restart (void) {
  circuit * root = subnet->getRoot ();
  for (circuit * c = root; c != NULL; c = (circuit *) c->getNext ()) {
    if (c->isNonLinear ()) c->restartDC ();
  }
}

/* Goes through the list of circuit objects and runs its initDC()
   function. */
void trsolver::initDC (void) {
  circuit * root = subnet->getRoot ();
  for (circuit * c = root; c != NULL; c = (circuit *) c->getNext ()) {
    c->initDC ();
  }
}

/* Goes through the list of circuit objects and runs its initTR()
   function. */
void trsolver::initTR (void) {
  char * IMethod = getPropertyString ("IntegrationMethod");
  nr_double_t start = getPropertyDouble ("Start");
  nr_double_t stop = getPropertyDouble ("Stop");
  nr_double_t points = getPropertyDouble ("Points");

  // fetch corrector integration method and determine predicor method
  corrMaxOrder = getPropertyInteger ("Order");
  corrType = CMethod = correctorType (IMethod, corrMaxOrder);
  predType = PMethod = predictorType (CMethod, corrMaxOrder, predMaxOrder);
  corrOrder = corrMaxOrder;
  predOrder = predMaxOrder;

  // initialize step values
  delta = getPropertyDouble ("InitialStep");
  deltaMin = getPropertyDouble ("MinStep");
  deltaMax = getPropertyDouble ("MaxStep");
  if (deltaMax == 0.0)
    deltaMax = MIN ((stop - start) / (points - 1), stop / 200);
  if (deltaMin == 0.0)
    deltaMin = NR_TINY * 10 * deltaMax;
  if (delta == 0.0)
    delta = MIN (stop / 200, deltaMax) / 10;
  if (delta < deltaMin) delta = deltaMin;
  if (delta > deltaMax) delta = deltaMax;

  // initialize step history
  setStates (2);
  initStates ();
  fillState (dState, delta);

  saveState (dState, deltas);
  setDelta ();
  calcCorrectorCoeff (corrType, corrOrder, corrCoeff, deltas);
  calcPredictorCoeff (predType, predOrder, predCoeff, deltas);

  // initialize history of solution vectors (solutions)
  for (int i = 0; i < 8; i++) {
    solution[i] = new tvector<nr_double_t>;
    setState (sState, (nr_double_t) i, i);
  }

  // tell circuits about the transient analysis
  circuit *c, * root = subnet->getRoot ();
  for (c = root; c != NULL; c = (circuit *) c->getNext ())
    initCircuitTR (c);
  // also initialize created circuits
  for (c = root; c != NULL; c = (circuit *) c->getPrev ())
    initCircuitTR (c);
}

// This function cleans up some memory used by the transient analysis.
void trsolver::deinitTR (void) {
  // cleanup solutions
  for (int i = 0; i < 8; i++) {
    delete solution[i];
    solution[i] = NULL;
  }
  // cleanup history
  if (tHistory) {
    delete tHistory;
    tHistory = NULL;
  }
}

// The function initialize a single circuit.
void trsolver::initCircuitTR (circuit * c) {
  c->initTR ();
  c->initStates ();
  c->setCoefficients (corrCoeff);
  c->setOrder (corrOrder);
  setIntegrationMethod (c, corrType);  
}

/* This function saves the results of a single solve() functionality
   (for the given timestamp) into the output dataset. */
void trsolver::saveAllResults (nr_double_t time) {
  vector * t;
  // add current frequency to the dependency of the output dataset
  if ((t = data->findDependency ("time")) == NULL) {
    t = new vector ("time");
    data->addDependency (t);
  }
  if (runs == 1) t->add (time);
  saveResults ("Vt", "It", 0, t);
}

/* This function is meant to adapt the current time-step the transient
   analysis advanced.  For the computation of the new time-step the
   truncation error depending on the integration method is used. */
nr_double_t trsolver::checkDelta (void) {
  nr_double_t LTEreltol = getPropertyDouble ("LTEreltol");
  nr_double_t LTEabstol = getPropertyDouble ("LTEabstol");
  nr_double_t LTEfactor = getPropertyDouble ("LTEfactor");
  nr_double_t dif, rel, tol, lte, q, n = NR_MAX;
  int N = countNodes ();
  int M = countVoltageSources ();

  // cec = corrector error constant
  nr_double_t cec = getCorrectorError (corrType, corrOrder);
  // pec = predictor error constant
  nr_double_t pec = getPredictorError (predType, predOrder);

  // go through each solution
  for (int r = 0; r < N + M; r++) {
    
    // skip real voltage sources
    if (r >= N) {
      if (findVoltageSource(r - N)->isVSource ())
	continue;
    }

    dif = x->get (r) - SOL(0)->get (r);
    if (isfinite (dif) && dif != 0) {
      // use Milne' estimate for the local truncation error
      rel = MAX (abs (x->get (r)), abs (SOL(0)->get (r)));
      tol = LTEreltol * rel + LTEabstol;
      lte = LTEfactor * (cec / (pec - cec)) * dif;
      q =  delta * exp (log (fabs (tol / lte)) / (corrOrder + 1));
      n = MIN (n, q);
    }
  }
#if STEPDEBUG
  logprint (LOG_STATUS, "DEBUG: delta according to local truncation "
	    "error h = %.3e\n", (double) n);
#endif
  delta = MIN ((n > 1.9 * delta) ? 2 * delta : delta, n);
  return delta;
}

// The function updates the integration coefficients.
void trsolver::updateCoefficients (nr_double_t delta) {
  setState (dState, delta);
  saveState (dState, deltas);
  calcCorrectorCoeff (corrType, corrOrder, corrCoeff, deltas);
  calcPredictorCoeff (predType, predOrder, predCoeff, deltas);
}
