/*
 *    This file is part of CasADi.
 *
 *    CasADi -- A symbolic framework for dynamic optimization.
 *    Copyright (C) 2010-2014 Joel Andersson, Joris Gillis, Moritz Diehl,
 *                            K.U. Leuven. All rights reserved.
 *    Copyright (C) 2011-2014 Greg Horn
 *
 *    CasADi is free software; you can redistribute it and/or
 *    modify it under the terms of the GNU Lesser General Public
 *    License as published by the Free Software Foundation; either
 *    version 3 of the License, or (at your option) any later version.
 *
 *    CasADi is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *    Lesser General Public License for more details.
 *
 *    You should have received a copy of the GNU Lesser General Public
 *    License along with CasADi; if not, write to the Free Software
 *    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */



#include "bonmin_interface.hpp"
#include "bonmin_nlp.hpp"
#include "casadi/core/std_vector_tools.hpp"
#include "../../core/profiling.hpp"
#include "../../core/casadi_options.hpp"
#include "../../core/casadi_interrupt.hpp"

#include <ctime>
#include <stdlib.h>
#include <iostream>
#include <iomanip>

using namespace std;

namespace casadi {
  timer BonminInterface::getTimerTime() {
    timer ret;
    ret.proc = clock();
    ret.wall = getRealTime();
    return ret;
  }

  // ret = t1 - t0
  diffTime BonminInterface::diffTimers(const timer t1, const timer t0) {
    diffTime ret;
    ret.proc = (t1.proc - t0.proc)/CLOCKS_PER_SEC;
    ret.wall = t1.wall - t0.wall;
    return ret;
  }

  // t += diff
  void BonminInterface::timerPlusEq(diffTime & t, const diffTime diff) {
    t.proc += diff.proc;
    t.wall += diff.wall;
  }

  // Convert a float to a string of an exact length.
  // First it tries fixed precision, then falls back to exponential notation.
  //
  // todo(jaeandersson,jgillis): needs either review or unit tests
  // because it throws exceptions if it fail.
  std::string formatFloat(double x, int totalWidth, int maxPrecision, int fallbackPrecision) {
    std::ostringstream out0;
    out0 << fixed << setw(totalWidth) << setprecision(maxPrecision) << x;
    std::string ret0 = out0.str();
    if (ret0.length() == totalWidth) {
      return ret0;
    } else if (ret0.length() > totalWidth) {
      std::ostringstream out1;
      out1 << setw(totalWidth) << setprecision(fallbackPrecision) << x;
      std::string ret1 = out1.str();
      if (ret1.length() != totalWidth)
        casadi_error(
          "bonmin timing formatting fallback is bugged, sorry about that."
          << "expected " << totalWidth <<  " digits, but got " << ret1.length()
          << ", string: \"" << ret1 << "\", number: " << x);
      return ret1;
    } else {
      casadi_error("bonmin timing formatting is bugged, sorry about that.");
    }
  }

  // Print out a beautiful timing summary.
  // Will print one row per tuple in the input vector.
  // The tuple must contain {name, # evals, total time in seconds}.
  void BonminInterface::timingSummary(
    std::vector<std::tuple<std::string, int, diffTime> > & xs) {
    // get padding of names
    int maxNameLen = 0;
    for (int k=0; k < xs.size(); ++k) {
      const int len = std::get<0>(xs[k]).length();
      maxNameLen = max(maxNameLen, len);
    }

    std::ostringstream out;
    std::string blankName(maxNameLen, ' ');
    out
      << blankName
      << "      proc           wall      num           mean             mean"
      << endl << blankName
      << "      time           time     evals       proc time        wall time"
      << endl;
    for (int k=0; k < xs.size(); ++k) {
      const std::tuple<std::string, int, diffTime> x = xs[k];
      const std::string name = std::get<0>(x);
      const int n = std::get<1>(x);
      const diffTime dt = std::get<2>(x);

      // don't print out this row if there were 0 calls
      if (n == 0)
        continue;

      out
        << setw(maxNameLen) << name << " "
        << formatFloat(dt.proc, 9, 3, 3) << " [s]  "
        << formatFloat(dt.wall, 9, 3, 3) << " [s]";
      if (n == -1) {
        // things like main loop don't have # evals
        out << endl;
      } else {
        out
          << " "
          << setw(5) << n;
        if (n < 2) {
          out << endl;
        } else {
          // only print averages if there is more than 1 eval
          out
            << " "
            << formatFloat(1000.0*dt.proc/n, 10, 2, 3) << " [ms]  "
            << formatFloat(1000.0*dt.wall/n, 10, 2, 3) << " [ms]"
            << endl;
        }
      }
    }
    // I'm worried that the following will set stdout stream state:
    // userOut() << out;

    // Just convert it to a string first to be safe.
    // todo(jaeandersson/jgillis): please review.
    const std::string ret = out.str();
    userOut() << ret;
  }

  extern "C"
  int CASADI_NLPSOLVER_BONMIN_EXPORT
  casadi_register_nlpsolver_bonmin(NlpSolverInternal::Plugin* plugin) {
    plugin->creator = BonminInterface::creator;
    plugin->name = "bonmin";
    plugin->doc = BonminInterface::meta_doc.c_str();
    plugin->version = 23;
    return 0;
  }

  extern "C"
  void CASADI_NLPSOLVER_BONMIN_EXPORT casadi_load_nlpsolver_bonmin() {
    NlpSolverInternal::registerPlugin(casadi_register_nlpsolver_bonmin);
  }

  BonminInterface::BonminInterface(const Function& nlp) : NlpSolverInternal(nlp) {
    addOption("pass_nonlinear_variables", OT_BOOLEAN, false);
    addOption("print_time",               OT_BOOLEAN, true,
              "print information about execution time");

    // Monitors
    addOption("monitor",                  OT_STRINGVECTOR, GenericType(),  "",
              "eval_f|eval_g|eval_jac_g|eval_grad_f|eval_h", true);

    // For passing metadata to BONMIN
    addOption("var_string_md",            OT_DICT, GenericType(),
              "String metadata (a dictionary with lists of strings) "
              "about variables to be passed to BONMIN");
    addOption("var_integer_md",           OT_DICT, GenericType(),
              "Integer metadata (a dictionary with lists of integers) "
              "about variables to be passed to BONMIN");
    addOption("var_numeric_md",           OT_DICT, GenericType(),
              "Numeric metadata (a dictionary with lists of reals) about "
              "variables to be passed to BONMIN");
    addOption("con_string_md",            OT_DICT, GenericType(),
              "String metadata (a dictionary with lists of strings) about "
              "constraints to be passed to BONMIN");
    addOption("con_integer_md",           OT_DICT, GenericType(),
              "Integer metadata (a dictionary with lists of integers) "
              "about constraints to be passed to BONMIN");
    addOption("con_numeric_md",           OT_DICT, GenericType(),
              "Numeric metadata (a dictionary with lists of reals) about "
              "constraints to be passed to BONMIN");
    addOption("discrete",           OT_BOOLVECTOR, GenericType(),
              "Indicates which of the variables are discrete, i.e. integer-valued");
    addOption("hessian_approximation", OT_STRING, "exact",
              "limited-memory|exact");

    char * default_solver = getenv("IPOPT_DEFAULT_LINEAR_SOLVER");
    if (default_solver) {
      setOption("linear_solver", default_solver);
    }

    // Start an BONMIN application
    Ipopt::SmartPtr<Bonmin::RegisteredOptions> roptions = new Bonmin::RegisteredOptions();
    BonminSetup::registerAllOptions(roptions);
    map<string, Ipopt::SmartPtr<Ipopt::RegisteredOption> > regops =
        roptions->RegisteredOptionsList();

    for (map<string, Ipopt::SmartPtr<Ipopt::RegisteredOption> >::const_iterator it=regops.begin();
        it!=regops.end();
        ++it) {
      // Option identifier
      string opt_name = it->first;

      // Short description goes here, even though we do have a longer description
      string opt_desc = it->second->ShortDescription();

      // Get the type
      Ipopt::RegisteredOptionType ipopt_type = it->second->Type();
      TypeID casadi_type;

      // Map Ipopt option category to a CasADi options type
      switch (ipopt_type) {
      case Ipopt::OT_Number:    casadi_type = OT_REAL;          break;
      case Ipopt::OT_Integer:   casadi_type = OT_INTEGER;       break;
      case Ipopt::OT_String:    casadi_type = OT_STRING;        break;
      case Ipopt::OT_Unknown:   continue; // NOTE: No mechanism to handle OT_Unknown options
      default:                  continue; // NOTE: Unknown Ipopt options category
      }

      addOption(opt_name, casadi_type, GenericType(), opt_desc);

      // Set default values of IPOPT options
      if (casadi_type == OT_REAL) {
        setDefault(opt_name, it->second->DefaultNumber());
      } else if (casadi_type == OT_INTEGER) {
        setDefault(opt_name, it->second->DefaultInteger());
      } else if (casadi_type == OT_STRING) {
        setDefault(opt_name, it->second->DefaultString());
      }

      // Save to map containing IPOPT specific options
      ops_[opt_name] = casadi_type;
    }
  }

  BonminInterface::~BonminInterface() {
  }

  void BonminInterface::init() {

    // Call the init method of the base class
    NlpSolverInternal::init();

    // Read user options
    exact_hessian_ = !hasSetOption("hessian_approximation") ||
        getOption("hessian_approximation")=="exact";

    if (hasSetOption("discrete")) {
      discrete_ = getOption("discrete");
      casadi_assert_message(discrete_.size()==nx_, "Length of 'discrete' option (" <<
        discrete_.size() <<  ") must match numbe rof variables (" << nx_ << ")");
    } else {
      discrete_ = std::vector<bool>(nx_, false);
    }

    // Get/generate required functions
    gradF();
    jacG();
    if (exact_hessian_) {
      hessLag();
    }

    if (verbose_) {
      userOut() << "There are " << nx_ << " variables and " << ng_ << " constraints." << endl;
      if (exact_hessian_) userOut() << "Using exact Hessian" << endl;
      else             userOut() << "Using limited memory Hessian approximation" << endl;
    }

  }

  void BonminInterface::evaluate() {

    if (CasadiOptions::profiling && CasadiOptions::profilingBinary) {
      profileWriteEntry(CasadiOptions::profilingLog, this);
    }

    if (inputs_check_) checkInputs();

    checkInitialBounds();



    if (gather_stats_) {
      Dict iterations;
      iterations["inf_pr"] = std::vector<double>();
      iterations["inf_du"] = std::vector<double>();
      iterations["mu"] = std::vector<double>();
      iterations["d_norm"] = std::vector<double>();
      iterations["regularization_size"] = std::vector<double>();
      iterations["obj"] = std::vector<double>();
      iterations["ls_trials"] = std::vector<int>();
      iterations["alpha_pr"] = std::vector<double>();
      iterations["alpha_du"] = std::vector<double>();
      iterations["obj"] = std::vector<double>();
      stats_["iterations"] = iterations;
    }

    // Reset the counters
    t_eval_f_ = t_eval_grad_f_ = t_eval_g_ = t_eval_jac_g_ = t_eval_h_ = t_callback_fun_ =
        t_callback_prepare_ = t_mainloop_ = {0, 0};

    n_eval_f_ = n_eval_grad_f_ = n_eval_g_ = n_eval_jac_g_ = n_eval_h_ =
        n_eval_callback_ = n_iter_ = 0;

    const timer time0 = getTimerTime();
    // MINLP instance
    SmartPtr<BonminUserClass> tminlp = new BonminUserClass(this);

    // Start an BONMIN application
    BonminSetup bonmin;

    bonmin.initializeOptionsAndJournalist();

    // Direct output through casadi::userOut()
    StreamJournal* jrnl_raw = new StreamJournal("console", J_ITERSUMMARY);
    jrnl_raw->SetOutputStream(&casadi::userOut());
    jrnl_raw->SetPrintLevel(J_DBG, J_NONE);
    SmartPtr<Journal> jrnl = jrnl_raw;
    bonmin.journalist()->AddJournal(jrnl);

    bool ret = true;
    // Pass all the options to bonmin
    for (map<string, TypeID>::const_iterator it=ops_.begin(); it!=ops_.end(); ++it)
      if (hasSetOption(it->first)) {
        GenericType op = getOption(it->first);
        switch (it->second) {
        case OT_REAL:
          ret &= bonmin.options()->SetNumericValue(it->first, op.toDouble(), false);
          break;
        case OT_INTEGER:
          ret &= bonmin.options()->SetIntegerValue(it->first, op.toInt(), false);
          break;
        case OT_STRING:
          ret &= bonmin.options()->SetStringValue(it->first, op.toString(), false);
          break;
        default:
          throw CasadiException("Illegal type");
        }
      }


    // Initialize
    bonmin.initialize(GetRawPtr(tminlp));

    // Branch-and-bound
    Bab bb;
    bb(bonmin);

    t_mainloop_ = diffTimers(getTimerTime(), time0);

    if (CasadiOptions::profiling && CasadiOptions::profilingBinary) {
      profileWriteExit(CasadiOptions::profilingLog, this,
        t_mainloop_.proc - (t_callback_fun_.proc-t_callback_prepare_.proc));
    }

    if (hasOption("print_time") && static_cast<bool>(getOption("print_time"))) {
      // Write timings
      std::vector<std::tuple<std::string, int, diffTime> > times;
      times.push_back(
        std::make_tuple("eval_f", n_eval_f_, t_eval_f_));
      times.push_back(
        std::make_tuple("eval_grad_f", n_eval_grad_f_, t_eval_grad_f_));
      times.push_back(
        std::make_tuple("eval_g", n_eval_g_, t_eval_g_));
      times.push_back(
        std::make_tuple("eval_jac_g", n_eval_jac_g_, t_eval_jac_g_));
      times.push_back(
        std::make_tuple("eval_h", n_eval_h_, t_eval_h_));

      // These guys get -1 calls to supress that part of the printout.
      diffTime t_all_functions = {0.0, 0.0};
      timerPlusEq(t_all_functions, t_eval_f_);
      timerPlusEq(t_all_functions, t_eval_grad_f_);
      timerPlusEq(t_all_functions, t_eval_g_);
      timerPlusEq(t_all_functions, t_eval_jac_g_);
      timerPlusEq(t_all_functions, t_eval_h_);
      times.push_back(
        std::make_tuple("all previous", -1, t_all_functions));
      diffTime t_bonmin;
      t_bonmin.proc = t_mainloop_.proc - t_all_functions.proc
        - t_callback_prepare_.proc - t_callback_fun_.proc;
      t_bonmin.wall = t_mainloop_.wall - t_all_functions.wall
        - t_callback_prepare_.wall - t_callback_fun_.wall;

      times.push_back(
        std::make_tuple("callback prep", n_eval_callback_, t_callback_prepare_));
      times.push_back(
        std::make_tuple("callback", n_eval_callback_, t_callback_fun_));
      times.push_back(
        std::make_tuple("bonmin", -1, t_bonmin));
      times.push_back(
        std::make_tuple("main loop", -1, t_mainloop_));

      // do the summary
      timingSummary(times);
    }


    stats_["t_eval_f.proc"] = t_eval_f_.proc;
    stats_["t_eval_grad_f.proc"] = t_eval_grad_f_.proc;
    stats_["t_eval_g.proc"] = t_eval_g_.proc;
    stats_["t_eval_jac_g.proc"] = t_eval_jac_g_.proc;
    stats_["t_eval_h.proc"] = t_eval_h_.proc;
    stats_["t_mainloop.proc"] = t_mainloop_.proc;
    stats_["t_callback_fun.proc"] = t_callback_fun_.proc;
    stats_["t_callback_prepare.proc"] = t_callback_prepare_.proc;

    stats_["t_eval_f.wall"] = t_eval_f_.wall;
    stats_["t_eval_grad_f.wall"] = t_eval_grad_f_.wall;
    stats_["t_eval_g.wall"] = t_eval_g_.wall;
    stats_["t_eval_jac_g.wall"] = t_eval_jac_g_.wall;
    stats_["t_eval_h.wall"] = t_eval_h_.wall;
    stats_["t_mainloop.wall"] = t_mainloop_.wall;
    stats_["t_callback_fun.wall"] = t_callback_fun_.wall;
    stats_["t_callback_prepare.wall"] = t_callback_prepare_.wall;

    stats_["n_eval_f"] = n_eval_f_;
    stats_["n_eval_grad_f"] = n_eval_grad_f_;
    stats_["n_eval_g"] = n_eval_g_;
    stats_["n_eval_jac_g"] = n_eval_jac_g_;
    stats_["n_eval_h"] = n_eval_h_;
    stats_["n_eval_callback"] = n_eval_callback_;

    stats_["iter_count"] = n_iter_-1;

  }

  bool BonminInterface::intermediate_callback(
      const double* x, const double* z_L, const double* z_U, const double* g,
      const double* lambda, double obj_value, int iter,
      double inf_pr, double inf_du, double mu, double d_norm,
      double regularization_size, double alpha_du, double alpha_pr, int ls_trials,
      bool full_callback) {
    n_iter_ += 1;
    try {
      log("intermediate_callback started");
      if (gather_stats_) {
        Dict iterations = stats_["iterations"];
        append_to_vec(iterations["inf_pr"], inf_pr);
        append_to_vec(iterations["inf_du"], inf_du);
        append_to_vec(iterations["mu"], mu);
        append_to_vec(iterations["d_norm"], d_norm);
        append_to_vec(iterations["regularization_size"], regularization_size);
        append_to_vec(iterations["alpha_pr"], alpha_pr);
        append_to_vec(iterations["alpha_du"], alpha_du);
        append_to_vec(iterations["ls_trials"], ls_trials);
        append_to_vec(iterations["obj"], obj_value);
        stats_["iterations"] = iterations;
      }
      const timer time0 = getTimerTime();
      if (!callback_.isNull()) {
        if (full_callback) {
          if (!output(NLP_SOLVER_X).isempty()) copy(x, x+nx_, output(NLP_SOLVER_X).begin());

          std::vector<double>& lambda_x = output(NLP_SOLVER_LAM_X).data();
          for (int i=0; i<lambda_x.size(); ++i) {
            lambda_x[i] = z_U[i]-z_L[i];
          }
          if (!output(NLP_SOLVER_LAM_G).isempty())
              copy(lambda, lambda+ng_, output(NLP_SOLVER_LAM_G).begin());
          if (!output(NLP_SOLVER_G).isempty()) copy(g, g+ng_, output(NLP_SOLVER_G).begin());
        } else {
          if (iter==0) {
            userOut<true, PL_WARN>()
              << "Warning: intermediate_callback is disfunctional in your installation. "
              "You will only be able to use getStats(). "
              "See https://github.com/casadi/casadi/wiki/enableBonminCallback to enable it."
              << endl;
          }
        }

        Dict iteration;
        iteration["iter"] = iter;
        iteration["inf_pr"] = inf_pr;
        iteration["inf_du"] = inf_du;
        iteration["mu"] = mu;
        iteration["d_norm"] = d_norm;
        iteration["regularization_size"] = regularization_size;
        iteration["alpha_pr"] = alpha_pr;
        iteration["alpha_du"] = alpha_du;
        iteration["ls_trials"] = ls_trials;
        iteration["obj"] = obj_value;
        stats_["iteration"] = iteration;

        output(NLP_SOLVER_F).at(0) = obj_value;

        n_eval_callback_ += 1;
        int ret = callback_(ref_, user_data_);

        const diffTime delta = diffTimers(getTimerTime(), time0);
        timerPlusEq(t_callback_fun_, delta);
        return  !ret;
      } else {
        return 1;
      }
    } catch(exception& ex) {
      if (getOption("iteration_callback_ignore_errors")) {
        userOut<true, PL_WARN>() << "intermediate_callback: " << ex.what() << endl;
      } else {
        throw ex;
      }
      return 1;
    }
  }

  void BonminInterface::finalize_solution(TMINLP::SolverReturn status,
      const double* x, double obj_value) {
    try {
      // Get primal solution
      if (x) copy(x, x+nx_, output(NLP_SOLVER_X).begin());

      // Get optimal cost
      output(NLP_SOLVER_F).at(0) = obj_value;

      // Get dual solution (simple bounds)
      std::vector<double>& lambda_x = output(NLP_SOLVER_LAM_X).data();
      for (int i=0; i<lambda_x.size(); ++i) {
        lambda_x[i] = std::numeric_limits<double>::quiet_NaN();
      }

      std::vector<double>& lambda = output(NLP_SOLVER_LAM_G).data();
      std::vector<double>& g = output(NLP_SOLVER_G).data();

      // Get dual solution (nonlinear bounds)
      fill(lambda.begin(), lambda.end(), std::numeric_limits<double>::quiet_NaN());

      // Get the constraints
      fill(g.begin(), g.end(), std::numeric_limits<double>::quiet_NaN());

      if (status == SUCCESS)
        stats_["return_status"] = "SUCCESS";
      if (status == MAXITER_EXCEEDED)
        stats_["return_status"] = "MAXITER_EXCEEDED";
      if (status == CPUTIME_EXCEEDED)
        stats_["return_status"] = "CPUTIME_EXCEEDED";
      if (status == STOP_AT_TINY_STEP)
        stats_["return_status"] = "STOP_AT_TINY_STEP";
      if (status == STOP_AT_ACCEPTABLE_POINT)
        stats_["return_status"] = "STOP_AT_ACCEPTABLE_POINT";
      if (status == LOCAL_INFEASIBILITY)
        stats_["return_status"] = "LOCAL_INFEASIBILITY";
      if (status == USER_REQUESTED_STOP)
        stats_["return_status"] = "USER_REQUESTED_STOP";
      if (status == FEASIBLE_POINT_FOUND)
        stats_["return_status"] = "FEASIBLE_POINT_FOUND";
      if (status == DIVERGING_ITERATES)
        stats_["return_status"] = "DIVERGING_ITERATES";
      if (status == RESTORATION_FAILURE)
        stats_["return_status"] = "RESTORATION_FAILURE";
      if (status == ERROR_IN_STEP_COMPUTATION)
        stats_["return_status"] = "ERROR_IN_STEP_COMPUTATION";
      if (status == INVALID_NUMBER_DETECTED)
        stats_["return_status"] = "INVALID_NUMBER_DETECTED";
      if (status == TOO_FEW_DEGREES_OF_FREEDOM)
        stats_["return_status"] = "TOO_FEW_DEGREES_OF_FREEDOM";
      if (status == INVALID_OPTION)
        stats_["return_status"] = "INVALID_OPTION";
      if (status == OUT_OF_MEMORY)
        stats_["return_status"] = "OUT_OF_MEMORY";
      if (status == INTERNAL_ERROR)
        stats_["return_status"] = "INTERNAL_ERROR";
    } catch(exception& ex) {
      userOut<true, PL_WARN>() << "finalize_solution failed: " << ex.what() << endl;
    }
  }


  bool BonminInterface::eval_h(const double* x, bool new_x, double obj_factor,
                             const double* lambda, bool new_lambda, int nele_hess,
                             int* iRow, int* jCol, double* values) {
    try {
      log("eval_h started");
      const timer time0 = getTimerTime();
      if (values == NULL) {
        int nz=0;
        const int* colind = hessLag_.output().colind();
        int ncol = hessLag_.output().size2();
        const int* row = hessLag_.output().row();
        for (int cc=0; cc<ncol; ++cc)
          for (int el=colind[cc]; el<colind[cc+1] && row[el]<=cc; ++el) {
            iRow[nz] = row[el];
            jCol[nz] = cc;
            nz++;
          }
      } else {
        // Pass the argument to the function
        hessLag_.setInputNZ(x, NL_X);
        hessLag_.setInput(input(NLP_SOLVER_P), NL_P);
        hessLag_.setInput(obj_factor, NL_NUM_IN+NL_F);
        hessLag_.setInputNZ(lambda, NL_NUM_IN+NL_G);

        // Evaluate
        hessLag_.evaluate();

        // Get results
        hessLag_.output().getSym(values);

        if (monitored("eval_h")) {
          userOut() << "x = " << hessLag_.input(NL_X).data() << endl;
          userOut() << "H = " << endl;
          hessLag_.output().printSparse();
        }

        if (regularity_check_ && !isRegular(hessLag_.output().data()))
            casadi_error("BonminInterface::h: NaN or Inf detected.");

      }
      const diffTime delta = diffTimers(getTimerTime(), time0);
      timerPlusEq(t_eval_h_, delta);
      if (CasadiOptions::profiling && CasadiOptions::profilingBinary) {
        profileWriteTime(CasadiOptions::profilingLog, this, 4, delta.proc);
      }
      n_eval_h_ += 1;
      log("eval_h ok");
      return true;
    } catch(exception& ex) {
      if (eval_errors_fatal_) throw ex;
      userOut<true, PL_WARN>() << "eval_h failed: " << ex.what() << endl;
      return false;
    }
  }

  bool BonminInterface::eval_jac_g(int n, const double* x, bool new_x, int m, int nele_jac,
                                  int* iRow, int *jCol, double* values) {
    try {
      log("eval_jac_g started");

      // Quich finish if no constraints
      if (m==0) {
        log("eval_jac_g quick return (m==0)");
        return true;
      }

      // Get function
      Function& jacG = this->jacG();

      const timer time0 = getTimerTime();
      if (values == NULL) {
        int nz=0;
        const int* colind = jacG.output().colind();
        int ncol = jacG.output().size2();
        const int* row = jacG.output().row();
        for (int cc=0; cc<ncol; ++cc)
          for (int el=colind[cc]; el<colind[cc+1]; ++el) {
            int rr = row[el];
            iRow[nz] = rr;
            jCol[nz] = cc;
            nz++;
          }
      } else {
        // Pass the argument to the function
        jacG.setInputNZ(x, NL_X);
        jacG.setInput(input(NLP_SOLVER_P), NL_P);

        // Evaluate the function
        jacG.evaluate();

        // Get the output
        jacG.getOutputNZ(values);

        if (monitored("eval_jac_g")) {
          userOut() << "x = " << jacG.input(NL_X).data() << endl;
          userOut() << "J = " << endl;
          jacG.output().printSparse();
        }
        if (regularity_check_ && !isRegular(jacG.output().data()))
            casadi_error("BonminInterface::jac_g: NaN or Inf detected.");
      }

      const diffTime delta = diffTimers(getTimerTime(), time0);
      timerPlusEq(t_eval_jac_g_, delta);
      if (CasadiOptions::profiling && CasadiOptions::profilingBinary) {
        profileWriteTime(CasadiOptions::profilingLog, this, 3, delta.proc);
      }
      n_eval_jac_g_ += 1;
      log("eval_jac_g ok");
      return true;
    } catch(exception& ex) {
      if (eval_errors_fatal_) throw ex;
      userOut<true, PL_WARN>() << "eval_jac_g failed: " << ex.what() << endl;
      return false;
    }
  }

  bool BonminInterface::eval_f(int n, const double* x, bool new_x, double& obj_value) {

    // Respond to a possible Crl+C signals
    InterruptHandler::check();

    try {

      log("eval_f started");

      // Log time
      const timer time0 = getTimerTime();
      casadi_assert(n == nx_);

      // Pass the argument to the function
      nlp_.setInputNZ(x, NL_X);
      nlp_.setInput(input(NLP_SOLVER_P), NL_P);

      // Evaluate the function
      nlp_.evaluate();

      // Get the result
      nlp_.getOutput(obj_value, NL_F);

      // Printing
      if (monitored("eval_f")) {
        userOut() << "x = " << nlp_.input(NL_X) << endl;
        userOut() << "obj_value = " << obj_value << endl;
      }

      if (regularity_check_ && !isRegular(nlp_.output(NL_F).data()))
          casadi_error("BonminInterface::f: NaN or Inf detected.");

      const diffTime delta = diffTimers(getTimerTime(), time0);
      timerPlusEq(t_eval_f_, delta);
      n_eval_f_ += 1;
      if (CasadiOptions::profiling && CasadiOptions::profilingBinary) {
        profileWriteTime(CasadiOptions::profilingLog, this, 0, delta.proc);
      }
      log("eval_f ok");
      return true;
    } catch(exception& ex) {
      if (eval_errors_fatal_) throw ex;
      userOut<true, PL_WARN>() << "eval_f failed: " << ex.what() << endl;
      return false;
    }

  }

  bool BonminInterface::eval_g(int n, const double* x, bool new_x, int m, double* g) {
    try {
      log("eval_g started");
      const timer time0 = getTimerTime();

      if (m>0) {
        // Pass the argument to the function
        nlp_.setInputNZ(x, NL_X);
        nlp_.setInput(input(NLP_SOLVER_P), NL_P);

        // Evaluate the function and tape
        nlp_.evaluate();

        // Ge the result
        nlp_.getOutputNZ(g, NL_G);

        // Printing
        if (monitored("eval_g")) {
          userOut() << "x = " << nlp_.input(NL_X) << endl;
          userOut() << "g = " << nlp_.output(NL_G) << endl;
        }
      }

      if (regularity_check_ && !isRegular(nlp_.output(NL_G).data()))
          casadi_error("BonminInterface::g: NaN or Inf detected.");

      const diffTime delta = diffTimers(getTimerTime(), time0);
      timerPlusEq(t_eval_g_, delta);
      if (CasadiOptions::profiling && CasadiOptions::profilingBinary) {
        profileWriteTime(CasadiOptions::profilingLog, this, 2, delta.proc);
      }
      n_eval_g_ += 1;
      log("eval_g ok");
      return true;
    } catch(exception& ex) {
      if (eval_errors_fatal_) throw ex;
      userOut<true, PL_WARN>() << "eval_g failed: " << ex.what() << endl;
      return false;
    }
  }

  bool BonminInterface::eval_grad_f(int n, const double* x, bool new_x, double* grad_f) {
    try {
      log("eval_grad_f started");
      const timer time0 = getTimerTime();
      casadi_assert(n == nx_);

      // Pass the argument to the function
      gradF_.setInputNZ(x, NL_X);
      gradF_.setInput(input(NLP_SOLVER_P), NL_P);

      // Evaluate, adjoint mode
      gradF_.evaluate();

      // Get the result
      gradF_.output().get(grad_f);

      // Printing
      if (monitored("eval_grad_f")) {
        userOut() << "x = " << gradF_.input(NL_X) << endl;
        userOut() << "grad_f = " << gradF_.output() << endl;
      }

      if (regularity_check_ && !isRegular(gradF_.output().data()))
          casadi_error("BonminInterface::grad_f: NaN or Inf detected.");

      const diffTime delta = diffTimers(getTimerTime(), time0);
      timerPlusEq(t_eval_grad_f_, delta);
      if (CasadiOptions::profiling && CasadiOptions::profilingBinary) {
        profileWriteTime(CasadiOptions::profilingLog, this, 1, delta.proc);
      }
      n_eval_grad_f_ += 1;
      log("eval_grad_f ok");
      return true;
    } catch(exception& ex) {
      if (eval_errors_fatal_) throw ex;
      userOut<true, PL_WARN>() << "eval_grad_f failed: " << ex.what() << endl;
      return false;
    }
  }

  bool BonminInterface::get_bounds_info(int n, double* x_l, double* x_u,
                                      int m, double* g_l, double* g_u) {
    try {
      casadi_assert(n == nx_);
      casadi_assert(m == ng_);
      input(NLP_SOLVER_LBX).getNZ(x_l);
      input(NLP_SOLVER_UBX).getNZ(x_u);
      input(NLP_SOLVER_LBG).getNZ(g_l);
      input(NLP_SOLVER_UBG).getNZ(g_u);
      return true;
    } catch(exception& ex) {
      userOut<true, PL_WARN>() << "get_bounds_info failed: " << ex.what() << endl;
      return false;
    }
  }

  bool BonminInterface::get_starting_point(int n, bool init_x, double* x,
                                         bool init_z, double* z_L, double* z_U,
                                         int m, bool init_lambda,
                                         double* lambda) {
    try {

      if (init_x) {
        input(NLP_SOLVER_X0).getNZ(x);
      }

      if (init_z) {
        // Get dual solution (simple bounds)
        std::vector<double>& lambda_x = input(NLP_SOLVER_LAM_X0).data();
        for (int i=0; i<lambda_x.size(); ++i) {
          z_L[i] = max(0., -lambda_x[i]);
          z_U[i] = max(0., lambda_x[i]);
        }
      }

      if (init_lambda) {
        input(NLP_SOLVER_LAM_G0).getNZ(lambda);
      }

      return true;
    } catch(exception& ex) {
      userOut<true, PL_WARN>() << "get_starting_point failed: " << ex.what() << endl;
      return false;
    }
  }

  void BonminInterface::get_nlp_info(int& n, int& m, int& nnz_jac_g, int& nnz_h_lag) {
    try {
      n = nx_;               // number of variables
      m = ng_;               // number of constraints

      // Get Jacobian sparsity pattern
      if (nlp_.output(NL_G).nnz()==0)
        nnz_jac_g = 0;
      else
        nnz_jac_g = jacG().output().nnz();

      // Get Hessian sparsity pattern
      if (exact_hessian_)
        nnz_h_lag = hessLag().output().sparsity().sizeU();
      else
        nnz_h_lag = 0;
    } catch(exception& ex) {
      userOut<true, PL_WARN>() << "get_nlp_info failed: " << ex.what() << endl;
    }
  }

  int BonminInterface::get_number_of_nonlinear_variables() {
    try {
      if (!static_cast<bool>(getOption("pass_nonlinear_variables"))) {
        // No Hessian has been interfaced
        return -1;
      } else {
        // Number of variables that appear nonlinearily
        int nv = 0;

        // Loop over the cols
        const Sparsity& spHessLag = this->spHessLag();
        const int* colind = spHessLag.colind();
        int ncol = spHessLag.size2();
        for (int i=0; i<ncol; ++i) {
          // If the col contains any non-zeros, the corresponding variable appears nonlinearily
          if (colind[i]!=colind[i+1])
            nv++;
        }

        // Return the number
        return nv;
      }
    } catch(exception& ex) {
      userOut<true, PL_WARN>() << "get_number_of_nonlinear_variables failed: " << ex.what() << endl;
      return -1;
    }
  }

  bool BonminInterface::get_list_of_nonlinear_variables(int num_nonlin_vars, int* pos_nonlin_vars) {
    try {
      // Running index
      int el = 0;

      // Loop over the cols
      const Sparsity& spHessLag = this->spHessLag();
      const int* colind = spHessLag.colind();
      int ncol = spHessLag.size2();
      for (int i=0; i<ncol; ++i) {
        // If the col contains any non-zeros, the corresponding variable appears nonlinearily
        if (colind[i]!=colind[i+1]) {
          pos_nonlin_vars[el++] = i;
        }
      }

      // Assert number and return
      casadi_assert(el==num_nonlin_vars);
      return true;
    } catch(exception& ex) {
      userOut<true, PL_WARN>() << "get_list_of_nonlinear_variables failed: " << ex.what() << endl;
      return false;
    }
  }

  void BonminInterface::setDefaultOptions(const std::vector<std::string>& recipes) {
    // Can be enabled when a new bugfixed version of Bonmin comes out
    //setOption("mehrotra_algorithm", "yes");
    //setOption("mu_oracle", "probing");
    for (int i=0;i<recipes.size();++i) {
      if (recipes[i]=="qp") {
        setOption("fixed_variable_treatment", "relax_bounds");
        setOption("jac_c_constant", "yes");
        setOption("jac_d_constant", "yes");
        setOption("hessian_constant", "yes");
      }
    }
  }

  DMatrix BonminInterface::getReducedHessian() {
#ifndef WITH_SBONMIN
    casadi_error("This feature requires sBONMIN support. Please consult the CasADi documentation.");
#else // WITH_SBONMIN
#ifndef WITH_CASADI_PATCH
    casadi_error("Retrieving the Hessian requires the CasADi sBONMIN patch. "
                 "Please consult the CasADi documentation.");
#else // WITH_CASADI_PATCH
    return red_hess_;
#endif // WITH_SBONMIN
#endif // WITH_CASADI_PATCH

  }

} // namespace casadi
