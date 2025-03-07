/*
 * SessionEnvironment.cpp
 *
 * Copyright (C) 2022 by Posit Software, PBC
 *
 * Unless you have received this program directly from Posit Software pursuant
 * to the terms of a commercial license agreement with Posit Software, then
 * this program is licensed to you under the terms of version 3 of the
 * GNU Affero General Public License. This program is distributed WITHOUT
 * ANY EXPRESS OR IMPLIED WARRANTY, INCLUDING THOSE OF NON-INFRINGEMENT,
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE. Please refer to the
 * AGPL (http://www.gnu.org/licenses/agpl-3.0.txt) for more details.
 *
 */

#include "SessionEnvironment.hpp"
#include "EnvironmentMonitor.hpp"

#include <algorithm>

#include <core/Exec.hpp>
#include <core/RecursionGuard.hpp>

#define INTERNAL_R_FUNCTIONS
#include <r/RJson.hpp>
#include <r/RSexp.hpp>
#include <r/RExec.hpp>
#include <r/session/RSession.hpp>
#include <r/RInterface.hpp>
#include <r/RRoutines.hpp>
#include <r/RCntxt.hpp>
#include <r/RCntxtUtils.hpp>
#include <session/SessionModuleContext.hpp>
#include <session/SessionSourceDatabase.hpp>
#include <session/SessionPersistentState.hpp>
#include <session/prefs/UserPrefs.hpp>

#include "EnvironmentUtils.hpp"

using namespace rstudio::core;
using namespace boost::placeholders;

namespace rstudio {
namespace session {
namespace modules { 
namespace environment {

// which language is currently active on the front-end? this state is
// synchronized between client and server so that we can tell when to
// active environment monitors for different languages
std::string s_environmentLanguage = kEnvironmentLanguageR;

// allocate on the heap so we control timing of destruction (if we leave it
// to the destructor we might release the underlying environment SEXP after
// R has already shut down)
EnvironmentMonitor* s_pEnvironmentMonitor = nullptr;

// which Python module is currently being monitored, if any?
std::string s_monitoredPythonModule;

// is the browser currently active? we store this state
// so that we can query this from R, without 'hiding' the
// browser state by pushing new contexts / frames on the stack
bool s_browserActive = false;

// are we currently monitoring the environment? this is almost always true, but can be 
// disabled by the user to help mitigate pathological cases in which environment monitoring
// has undesirable side effects.
bool s_monitoring = true;

namespace {

// Keeps track of the data related to the most recent debugging event
class LineDebugState
{
   public:
      LineDebugState()
      {
         reset();
      }
      void reset()
      {
         lastDebugText = "";
         lastDebugLine = 0;
      }
      std::string lastDebugText;
      int lastDebugLine;
};

// The environment monitor and friends do work in response to events in R.
// In rare cases, this work can trigger the same events in R that are
// being responded to, leading to unwanted recursion. This simple guard
// increments the given counter on construction (and decrements on destruction)
// so vulnerable event handlers below can prevent reentrancy.
class EventRecursionGuard
{
public:
   EventRecursionGuard(int& counter): counter_(counter) { counter_++; }
   ~EventRecursionGuard() { counter_--; }
private:
   int& counter_;
};

bool isValidSrcref(SEXP srcref)
{
   return srcref && TYPEOF(srcref) != NILSXP;
}

bool handleRBrowseEnv(const core::FilePath& filePath)
{
   if (filePath.getFilename() == "wsbrowser.html")
   {
      module_context::showContent("R objects", filePath);
      return true;
   }
   else
   {
      return false;
   }
}

bool hasExternalPtrImpl(SEXP obj, bool nullPtr, std::set<SEXP>& visited)
{
   // if we've already visited this SEXP, bail
   if (visited.count(obj))
      return false;

   // mark SEXP as visited
   visited.insert(obj);

   // list the contents of this environment
   std::vector<r::sexp::Variable> vars;
   r::sexp::Protect rProtect;
   if (TYPEOF(obj) == S4SXP)
   {
      // for S4 objects, list the attributes (which correspond to slots)
      r::sexp::listNamedAttributes(obj, &rProtect, &vars);
   }
   else
   {
      // not S4, coerce to environment
      SEXP envir = R_NilValue;
      if (TYPEOF(obj) == ENVSXP)
      {
         // we were given a primitive environment (ENVSXP)
         envir = obj;
      }
      else
      {
         // convert the passed environment into a primitive environment; this is required so that
         // e.g. reference objects that subclass 'environment' can be introspected below
         Error error = r::sexp::asPrimitiveEnvironment(obj, &envir, &rProtect);
         if (error)
         {
            // can't search in here
            return false;
         }
      }

      r::sexp::listEnvironment(envir,
                               true,  // include all values
                               false, // don't include last dot
                               &rProtect, &vars);
   }

   // check for external pointers
   for (std::vector<r::sexp::Variable>::iterator it = vars.begin(); it != vars.end(); it++)
   {
      if (r::sexp::isExternalPointer(it->second) &&
          r::sexp::isNullExternalPointer(it->second) == nullPtr)
      {
         return true;
      }

      if (r::sexp::isPrimitiveEnvironment(it->second) || TYPEOF(it->second) == S4SXP)
      {
         // if this object is itself an environment, check it recursively for external pointers.
         // (we do this only if there's sufficient recursion depth remaining)
         if (hasExternalPtrImpl(it->second, nullPtr, visited))
            return true;
      }
   }

   return false;
}


bool hasExternalPtr(SEXP obj,      // environment to search for external pointers
                    bool nullPtr)  // whether to look for NULL pointers
{
   std::set<SEXP> visited;
   return hasExternalPtrImpl(obj, nullPtr, visited);
}

SEXP rs_hasExternalPointer(SEXP objSEXP, SEXP nullSEXP)
{
   bool nullPtr = r::sexp::asLogical(nullSEXP);
   r::sexp::Protect protect;
   bool hasPtr = false;
   if (r::sexp::isExternalPointer(objSEXP))
   {
      // object is an external pointer itself
      hasPtr = r::sexp::isNullExternalPointer(objSEXP) == nullPtr;
   }
   else if (r::sexp::isPrimitiveEnvironment(objSEXP) || TYPEOF(objSEXP) == S4SXP)
   {
      // object is an environment; check it for external pointers
      hasPtr = hasExternalPtr(objSEXP, nullPtr);
   }
   return r::sexp::create(hasPtr, &protect);
}

// Does an object contain an ALTREP anywhere? ALTREP (alternative representation) objects often
// require special treatment.
SEXP rs_hasAltrep(SEXP obj)
{
   r::sexp::Protect protect;
   return r::sexp::create(hasAltrep(obj), &protect);
}

// Is an object an R ALTREP object? 
SEXP rs_isAltrep(SEXP obj)
{
   r::sexp::Protect protect;
   return r::sexp::create(isAltrep(obj), &protect);
}

// Construct a simulated source reference from a context containing a
// function being debugged, and either the context containing the current
// invocation or a string containing the last debug output from R.
// We use this to highlight portions of deparsed functions when visually
// stepping through code for which source references are unavailable.
SEXP simulatedSourceRefsOfContext(const r::context::RCntxt& context,
                                  const r::context::RCntxt& lineContext,
                                  const LineDebugState* pLineDebugState)
{
   SEXP simulatedSrcref = R_NilValue;
   r::sexp::Protect protect;
   // The objects we will later transmit to .rs.simulateSourceRefs below
   // include language objects that we need to protect from early evaluation.
   // Attach them to a carrier SEXP as attributes rather than passing directly.
   SEXP info = r::sexp::create("_rs_sourceinfo", &protect);
   r::sexp::setAttrib(info, "_rs_callfun", context.callfun());
   if (lineContext)
      r::sexp::setAttrib(info, "_rs_callobj", lineContext.call());
   else if (pLineDebugState != nullptr)
   {
      SEXP lastDebugSEXP = r::sexp::create(
               pLineDebugState->lastDebugText, &protect);
      r::sexp::setAttrib(info, "_rs_calltext", lastDebugSEXP);
      SEXP lastLineSEXP = r::sexp::create(
               pLineDebugState->lastDebugLine, &protect);
      r::sexp::setAttrib(info, "_rs_lastline", lastLineSEXP);
   }
   Error error = r::exec::RFunction(".rs.simulateSourceRefs", info)
         .call(&simulatedSrcref, &protect);
   if (error)
      LOG_ERROR(error);
   return simulatedSrcref;
}

// Return the call frames and debug information as a JSON object.
json::Array callFramesAsJson(LineDebugState* pLineDebugState)
{
   r::context::RCntxt::iterator context = r::context::RCntxt::begin();
   r::context::RCntxt prevContext = *context;
   r::context::RCntxt srcContext = *context;
   json::Array listFrames;
   int contextDepth = 0;
   Error error;
   std::map<SEXP,r::context::RCntxt> envSrcrefCtx;

   for (; context != r::context::RCntxt::end(); context++)
   {
      // if this context has a valid srcref, use it to supply the srcrefs for
      // debugging in the environment of the callee. note that there may be
      // multiple srcrefs on the stack for a given closure; in this case we
      // always want to take the first one as it's the most current/specific.
      if (isValidSrcref(context->contextSourceRefs()) &&
          !context->nextcontext().isNull())
      {
         SEXP env = context->nextcontext().cloenv();
         if (envSrcrefCtx.find(env) == envSrcrefCtx.end())
            envSrcrefCtx[env] = *context;
      }

      if (context->callflag() & CTXT_FUNCTION)
      {
         json::Object varFrame;
         std::string functionName;
         varFrame["context_depth"] = ++contextDepth;

         error = context->functionName(&functionName);
         if (error)
         {
            LOG_ERROR(error);
         }
         varFrame["function_name"] = functionName;
         varFrame["is_error_handler"] = context->isErrorHandler();
         varFrame["is_hidden"] = context->isDebugHidden();

         // attempt to find the refs for the source that invoked this function;
         // use our own refs otherwise
         std::map<SEXP,r::context::RCntxt>::iterator srcCtx =
            envSrcrefCtx.find(context->cloenv());
         if (srcCtx != envSrcrefCtx.end())
            srcContext = srcCtx->second;
         else
            srcContext = *context;

         // extract source reference associated with source context
         SEXP srcref = srcContext.contextSourceRefs();
         
         // mark this as a source-equivalent function if it's evaluating user
         // code into the global environment
         varFrame["is_source_equiv"] =
               context->cloenv() == R_GlobalEnv &&
               isValidSrcref(srcref);

         std::string filename;
         error = srcContext.fileName(&filename);
         if (error)
            LOG_ERROR(error);
         
         varFrame["file_name"] = filename;
         varFrame["aliased_file_name"] =
               module_context::createAliasedPath(FilePath(filename));

         if (isValidSrcref(srcref))
         {
            varFrame["real_sourceref"] = true;
            sourceRefToJson(srcref, &varFrame);
         }
         else
         {
            varFrame["real_sourceref"] = false;
            // if this is the top frame, we simulate the sourceref using R
            // output of the last debugged statement; if it isn't, we
            // construct it by deparsing calls in the context stack.
            SEXP simulatedSrcref;
            if (contextDepth == 1 &&
                pLineDebugState != nullptr &&
                pLineDebugState->lastDebugText.length() > 0)
               simulatedSrcref =
                     simulatedSourceRefsOfContext(*context, 
                           r::context::RCntxt(), pLineDebugState);
            else
               simulatedSrcref =
                     simulatedSourceRefsOfContext(*context, prevContext,
                                                  nullptr);

            // store the line stepped over in the top frame, so we can infer
            // that the next line stepped over will be near this one
            if (contextDepth == 1 &&
                pLineDebugState != nullptr &&
                isValidSrcref(simulatedSrcref))
            {
               int stepLine = INTEGER(simulatedSrcref)[0];
               if (stepLine > 0)
                  pLineDebugState->lastDebugLine = stepLine;
            }

            sourceRefToJson(simulatedSrcref, &varFrame);
         }

         // extract the first line of the function. the client can optionally
         // use this to compute the source location as an offset into the
         // function rather than as an absolute file position (useful when
         // we need to debug a copy of the function rather than the real deal).
         SEXP srcRef = context->callFunSourceRefs();
         if (isValidSrcref(srcRef) && TYPEOF(srcRef) == INTSXP)
         {
            varFrame["function_line_number"] = INTEGER(srcRef)[0];
         }
         else
         {
            // if we don't have a source ref, we'll debug using a deparsed
            // version of the function that starts on line 1
            varFrame["function_line_number"] = 1;
         }

         std::string callSummary;
         error = context->callSummary(&callSummary);
         if (error)
            LOG_ERROR(error);

         varFrame["call_summary"] = error ? "" : callSummary;

         // If this is a Shiny function, provide its label
         varFrame["shiny_function_label"] = context->shinyFunctionLabel();

         listFrames.push_back(varFrame);
      }
      prevContext = *context;
   }
   return listFrames;
}

json::Array environmentListAsJson()
{
    using namespace rstudio::r::sexp;
    Protect rProtect;
    std::vector<Variable> vars;
    json::Array listJson;

    if (s_pEnvironmentMonitor->hasEnvironment())
    {
       SEXP env = s_pEnvironmentMonitor->getMonitoredEnvironment();
       listEnvironment(env,
                       false,
                       prefs::userPrefs().showLastDotValue(),
                       &rProtect,
                       &vars);

       // get object details and transform to json
       std::transform(vars.begin(),
                      vars.end(),
                      std::back_inserter(listJson),
                      boost::bind(varToJson, env, _1));
    }

    return listJson;
}

Error listEnvironment(boost::shared_ptr<int> pContextDepth,
                      const json::JsonRpcRequest&,
                      json::JsonRpcResponse* pResponse)
{
   // return list
   pResponse->setResult(environmentListAsJson());
   return Success();
}

// Sets an environment by name. Used when the environment can be reliably
// identified by its name (e.g. package environments).
Error setEnvironmentName(int contextDepth,
                         const r::context::RCntxt &context,
                         std::string environmentName)
{
   SEXP environment = R_GlobalEnv;
   if (environmentName == "R_GlobalEnv")
   {
      environment = R_GlobalEnv;
   }
   else if (environmentName == "base")
   {
      environment = R_BaseEnv;
   }
   else
   {
      r::sexp::Protect protect;
      // We need to traverse the search path manually looking for an environment
      // whose name matches the one the caller requested, because R's
      // as.environment() function only searches the global search path, and
      // we may wish to set an environment whose name only exists in a private
      // environment chain.
      //
      // This would be better wrapped in an R function, but this code may
      // run during session init when tools:rstudio isn't yet attached to the
      // search path.
      SEXP env = contextDepth > 0 && context ?
                        context.cloenv() :
                        R_GlobalEnv;
      std::string candidateEnv;
      Error error;
      while (env != R_EmptyEnv)
      {
         error = r::exec::RFunction("environmentName", env).call(&candidateEnv);
         if (error)
            break;
         if (candidateEnv == environmentName)
         {
            environment = env;
            break;
         }
         // Proceed to the parent of the environment
         env = ENCLOS(env);
      }
      if (error || env == R_EmptyEnv)
      {
         s_pEnvironmentMonitor->setMonitoredEnvironment(R_GlobalEnv, true);
         return error;
      }
   }

   s_pEnvironmentMonitor->setMonitoredEnvironment(environment, true);
   return Success();
}

Error setEnvironment(boost::shared_ptr<int> pContextDepth,
                     boost::shared_ptr<r::context::RCntxt> pCurrentContext,
                     const json::JsonRpcRequest& request,
                     json::JsonRpcResponse* pResponse)
{
   std::string environmentName;
   Error error = json::readParam(request.params, 0, &environmentName);
   if (error)
      return error;

   s_monitoredPythonModule = std::string();
   
   if (s_environmentLanguage == kEnvironmentLanguageR)
   {
      error = setEnvironmentName(*pContextDepth,
                                 *pCurrentContext,
                                 environmentName);
   }
   else if (s_environmentLanguage == kEnvironmentLanguagePython)
   {
      if (environmentName != s_monitoredPythonModule)
      {
         s_monitoredPythonModule = environmentName;
         
         ClientEvent event(client_events::kEnvironmentRefresh);
         module_context::enqueClientEvent(event);
      }
   }
   else
   {
      LOG_WARNING_MESSAGE("Unexpected language '" + s_environmentLanguage + "'");
   }
   
   if (error)
      return error;

   persistentState().setActiveEnvironmentName(environmentName);
   return Success();
}

// Sets an environment by its frame number. Used for unnamed, transient
// function environments.
Error setEnvironmentFrame(const json::JsonRpcRequest& request,
                          json::JsonRpcResponse* pResponse)
{
   int frameNumber = 0;
   Error error = json::readParam(request.params, 0, &frameNumber);
   if (error)
      return error;

   SEXP environment;
   r::sexp::Protect protect;
   error = r::exec::RFunction("sys.frame", frameNumber)
            .call(&environment, &protect);
   if (error)
      return error;

   s_pEnvironmentMonitor->setMonitoredEnvironment(environment, true);
   return Success();
}

// given a function context, indicate whether the copy of the source code
// for the function is different than the source code on disk.
bool functionIsOutOfSync(const r::context::RCntxt& context,
                         std::string *pFunctionCode)
{
   Error error;
   r::sexp::Protect protect;
   SEXP sexpCode = R_NilValue;

   // start by extracting the source code from the call site
   error = r::exec::RFunction(".rs.deparseFunction")
         .addParam(context.originalFunctionCall())
         .addParam(true)
         .addParam(true)
         .call(&sexpCode, &protect);
   
   if (error)
   {
      LOG_ERROR(error);
      return true;
   }

   error = r::sexp::extract(sexpCode, pFunctionCode, true);
   if (error)
   {
      LOG_ERROR(error);
      return true;
   }

   // make sure the function has source references
   if (!context.hasSourceRefs())
   {
      return true;
   }

   return functionDiffersFromSource(context.callFunSourceRefs(), *pFunctionCode);
}

// Returns a JSON array containing the names and associated call frame numbers
// of the current environment stack.
json::Value environmentNames(SEXP env)
{
   SEXP environments;
   r::sexp::Protect protect;
   Error error = r::exec::RFunction(".rs.environmentList", env)
                                    .call(&environments, &protect);
   if (error)
   {
      LOG_ERROR(error);
      return json::Array();
   }
   else
   {
      json::Value namesJson;
      error = r::json::jsonValueFromObject(environments, &namesJson);
      if (error)
      {
         LOG_ERROR(error);
         return json::Array();
      }
      return namesJson;
   }
}

json::Object pythonEnvironmentStateData(const std::string& environment)
{
   SEXP state = R_NilValue;
   r::sexp::Protect protect;
   Error error =
         r::exec::RFunction(".rs.reticulate.environmentState")
         .addParam(environment)
         .call(&state, &protect);
   
   if (error)
   {
      LOG_ERROR(error);
      return json::Object();
   }
   
   json::Object jsonState;
   error = r::json::jsonValueFromObject(state, &jsonState);
   if (error)
   {
      LOG_ERROR(error);
      return json::Object();
   }
   
   jsonState["environment_monitoring"] = s_monitoring;
   return jsonState;
}

// create a JSON object that contains information about the current environment;
// used both to initialize the environment state on first load and to send
// information about the new environment on a context change
json::Object commonEnvironmentStateData(
   int depth,
   bool includeContents,
   LineDebugState* pLineDebugState)
{
   json::Object varJson;
   bool useProvidedSource = false;
   std::string functionCode;
   bool inFunctionEnvironment = false;

   // emit the current list of values in the environment, but only if not monitoring (as the intent
   // of the monitoring switch is to avoid implicit environment listing)
   varJson["environment_monitoring"] = s_monitoring;
   varJson["environment_list"] = includeContents ? environmentListAsJson() : json::Array();
   
   varJson["context_depth"] = depth;
   varJson["call_frames"] = callFramesAsJson(pLineDebugState);
   varJson["function_name"] = "";

   // if we're in a debug context, add information about the function currently
   // being debugged
   if (depth > 0)
   {
      r::context::RCntxt context = r::context::getFunctionContext(depth);
      if (context)
      {
         std::string functionName;
         Error error = context.functionName(&functionName);
         if (error)
            LOG_ERROR(error);

         // If the environment currently monitored is the function's
         // environment, return that environment, unless the environment is the
         // global environment (which happens for source-equivalent functions).
         SEXP env = s_pEnvironmentMonitor->getMonitoredEnvironment();
         if (env != R_GlobalEnv && env == context.cloenv())
         {
            varJson["environment_name"] = functionName + "()";
            std::string envLocation;
            error = r::exec::RFunction(".rs.environmentName", 
                  ENCLOS(context.cloenv())).call(&envLocation);
            if (error)
               LOG_ERROR(error);
            varJson["function_environment_name"] = envLocation;
            varJson["environment_is_local"] = true;
            inFunctionEnvironment = true;
         }

         // The eval and evalq functions receive special treatment since they
         // evaluate code from elsewhere (they don't have meaningful bodies we
         // can test here)
         if (functionName != "eval" && functionName != "evalq")
         {
            // see if the function to be debugged is out of sync with its saved
            // sources (if available).
            useProvidedSource =
                  functionIsOutOfSync(context, &functionCode) &&
                  functionCode != "NULL";
         }
         varJson["function_name"] = functionName;
      }
   }

   if (!inFunctionEnvironment)
   {
      // emit the name of the environment we're currently working with
      std::string environmentName;
      bool local = false;
      if (s_pEnvironmentMonitor->hasEnvironment())
      {
         Error error = r::exec::RFunction(".rs.environmentName",
                                    s_pEnvironmentMonitor->getMonitoredEnvironment())
                                    .call(&environmentName);
         if (error)
            LOG_ERROR(error);

         error = r::exec::RFunction(".rs.environmentIsLocal",
                                    s_pEnvironmentMonitor->getMonitoredEnvironment())
                                    .call(&local);
         if (error)
            LOG_ERROR(error);
      }
      varJson["environment_name"] = environmentName;
      varJson["environment_is_local"] = local;
   }

   // always emit the code for the function, even if we don't think that the
   // client's going to need it. we only checked the saved copy of the function
   // above; the client may be aware of local/unsaved changes to the function,
   // in which case it will need to fall back on a server-provided copy.
   varJson["use_provided_source"] = useProvidedSource;
   varJson["function_code"] = functionCode;

   return varJson;
}

void enqueContextDepthChangedEvent(int depth,
                                   LineDebugState* pLineDebugState)
{
   // emit an event to the client indicating the new call frame and the
   // current state of the environment
   ClientEvent event (client_events::kContextDepthChanged,
                      commonEnvironmentStateData(depth, s_monitoring, pLineDebugState));
   module_context::enqueClientEvent(event);
}

void enqueBrowserLineChangedEvent(const SEXP srcref)
{
   json::Object varJson;
   sourceRefToJson(srcref, &varJson);
   ClientEvent event (client_events::kBrowserLineChanged, varJson);
   module_context::enqueClientEvent(event);
}

Error setContextDepth(boost::shared_ptr<int> pContextDepth,
                      boost::shared_ptr<LineDebugState> pLineDebugState,
                      const json::JsonRpcRequest& request,
                      json::JsonRpcResponse*)
{
   // get the requested depth
   int requestedDepth;
   Error error = json::readParam(request.params, 0, &requestedDepth);
   if (error)
      return error;

   // set state for the new depth
   *pContextDepth = requestedDepth;
   SEXP env = nullptr;
   r::context::getFunctionContext(requestedDepth, nullptr, &env);
   s_pEnvironmentMonitor->setMonitoredEnvironment(env);

   // populate the new state on the client
   enqueContextDepthChangedEvent(*pContextDepth, pLineDebugState.get());

   return Success();
}

Error getEnvironmentState(boost::shared_ptr<int> pContextDepth,
                          boost::shared_ptr<LineDebugState> pLineDebugState,
                          const json::JsonRpcRequest& request,
                          json::JsonRpcResponse* pResponse)
{
   std::string language = kEnvironmentLanguageR;
   std::string environment = "R_GlobalEnv";
   Error error = json::readParams(request.params, &language, &environment);
   if (error)
      LOG_ERROR(error);
   
   json::Object jsonState;
   
   if (language == kEnvironmentLanguageR)
   {
      jsonState = commonEnvironmentStateData(
               *pContextDepth,
               true,
               pLineDebugState.get());
   }
   else if (language == kEnvironmentLanguagePython)
   {
      jsonState = pythonEnvironmentStateData(environment);
   }
   else
   {
      LOG_WARNING_MESSAGE("Unexpected language '" + language + "'");
   }
   
   pResponse->setResult(jsonState);
   return Success();
}

void onDetectChanges(module_context::ChangeSource /* source */)
{
   // Prevent recursive calls to this function
   DROP_RECURSIVE_CALLS;

   // Ignore if not monitoring
   if (!s_monitoring)
      return;

   // This operation may use the R runtime so don't run it if this RPC was run in the offline thread
   if (!core::thread::isMainThread())
      return;

   // Check for Python changes
   if (s_environmentLanguage == kEnvironmentLanguagePython &&
       !s_monitoredPythonModule.empty())
   {
      Error error =
            r::exec::RFunction(".rs.reticulate.detectChanges")
            .addParam(s_monitoredPythonModule)
            .call();
      
      if (error)
         LOG_ERROR(error);
   }
   
   s_pEnvironmentMonitor->checkForChanges();
   
}

void onConsolePrompt(boost::shared_ptr<int> pContextDepth,
                     boost::shared_ptr<LineDebugState> pLineDebugState,
                     boost::shared_ptr<bool> pCapturingDebugOutput,
                     boost::shared_ptr<r::context::RCntxt> pCurrentContext)
{
   // Prevent recursive calls to this function
   DROP_RECURSIVE_CALLS;

   int depth = 0;
   SEXP environmentTop = nullptr;
   r::context::RCntxt context;

   // End debug output capture every time a console prompt occurs
   *pCapturingDebugOutput = false;

   // If we were debugging but there's no longer a browser on the context stack,
   // switch back to the top level; otherwise, examine the stack and find the
   // first function there running user code.
   s_browserActive = r::context::inBrowseContext();
   if (*pContextDepth > 0 && !s_browserActive)
   {
      context = r::context::globalContext();
      environmentTop = R_GlobalEnv;
   }
   else
   {
      // If we're not currently debugging, look for user code (we prefer to
      // show the user their own code on entering debug), but once debugging,
      // allow the user to explore other code.
      context = r::context::getFunctionContext(BROWSER_FUNCTION,
                                               &depth, &environmentTop);
   }

   if (environmentTop != s_pEnvironmentMonitor->getMonitoredEnvironment() ||
       depth != *pContextDepth ||
       context != *pCurrentContext)
   {
      // if we appear to be switching into debug mode, make sure there's a
      // browser call somewhere on the stack. if there isn't, then we're
      // probably just waiting for user input inside a function (e.g. scan());
      // assume the user isn't interested in seeing the function's internals.
      if (*pContextDepth == 0 && !s_browserActive)
      {
         return;
      }

      // if we're leaving debug mode, clear out the debug state to prepare
      // for the next debug session
      if (*pContextDepth > 0 && depth == 0)
      {
         pLineDebugState->reset();
      }

      // start monitoring the environment at the new depth
      s_pEnvironmentMonitor->setMonitoredEnvironment(environmentTop);
      *pContextDepth = depth;
      *pCurrentContext = context;
      enqueContextDepthChangedEvent(depth, pLineDebugState.get());
   }
   // if we're debugging and stayed in the same frame, update the line number
   else if (depth > 0)
   {
      // we don't want to send linenumber updates if the current depth is inside
      // a debug-hidden function
      if (!r::context::inDebugHiddenContext())
      {
         // check to see if we have real source references for the currently
         // executing context
         SEXP srcref = r::context::globalContext().srcref();
         if (!isValidSrcref(srcref))
         {
            // we don't, so reconstruct them from R output
            r::context::RCntxt firstFunContext =
                  r::context::firstFunctionContext();
            srcref = simulatedSourceRefsOfContext(firstFunContext, 
                  r::context::RCntxt(), pLineDebugState.get());
         }
         enqueBrowserLineChangedEvent(srcref);
      }
   }
}

void onBeforeExecute()
{
   // The client tracks busy state based on whether a console prompt has
   // been issued (because R doesn't reliably deliver non-busy state) --
   // i.e. when a console prompt occurs the client leaves busy state.
   // During debugging the busy state is therefore exited as soon as a
   // Browse> prompt is hit. This is often not a problem as the debug
   // stop command will interrupt R if necessary. However, in the case
   // where the Next or Continue command results in R running without
   // hitting another breakpoint we've essentially lost the busy state.
   //
   // This handler (which executes right before console input is returned
   // to R) checks whether we are in the Browser and if so re-raises the
   // busy event to indicate that R is now back in a busy state. The busy
   // state will be immediately cleared if another Browse> prompt is hit
   // however if R continues running then the client will properly restore
   // the state of the interruptR command

   s_browserActive = r::context::inBrowseContext();
   if (s_browserActive)
   {
      ClientEvent event(client_events::kBusy, true);
      module_context::enqueClientEvent(event);
   }
}

Error getEnvironmentNames(boost::shared_ptr<int> pContextDepth,
                          boost::shared_ptr<r::context::RCntxt> pCurrentContext,
                          const json::JsonRpcRequest& request,
                          json::JsonRpcResponse* pResponse)
{
   std::string language;
   Error error = json::readParams(request.params, &language);
   if (error)
   {
      LOG_ERROR(error);
      return Success();
   }
   
   if (language == kEnvironmentLanguagePython)
   {
      Error error;
      
      SEXP environments = R_NilValue;
      r::sexp::Protect protect;
      error = r::exec::RFunction(".rs.reticulate.listLoadedModules")
            .call(&environments, &protect);
      
      if (error)
      {
         LOG_ERROR(error);
         return Success();
      }
      
      json::Value environmentsJson;
      error = r::json::jsonValueFromObject(environments, &environmentsJson);
      if (error)
      {
         LOG_ERROR(error);
         return Success();
      }
      
      pResponse->setResult(environmentsJson);
      return Success();
   }
   else if (language == kEnvironmentLanguageR)
   {
      // If looking at a non-toplevel context, start from there; otherwise, start
      // from the global environment.
      SEXP env = *pContextDepth > 0 ?
               pCurrentContext->cloenv() :
               R_GlobalEnv;
      pResponse->setResult(environmentNames(env));
   }
   else
   {
      LOG_WARNING_MESSAGE("Unexpected language '" + language + "'");
   }
   
   return Success();
}

void initEnvironmentMonitoring()
{
   // Restore monitoring state
   s_monitoring = persistentState().environmentMonitoring();

   // Check to see whether we're actively debugging. If we are, the debug
   // environment trumps whatever the user wants to browse in at the top level.
   int contextDepth = 0;
   r::context::RCntxt context = r::context::getFunctionContext(
            BROWSER_FUNCTION, &contextDepth);
   if (contextDepth == 0 || !r::context::inBrowseContext())
   {
      // Not actively debugging; see if we have a stored environment name to
      // begin monitoring.
      std::string envName = persistentState().activeEnvironmentName();
      if (!envName.empty())
      {
         // It's possible for this to fail if the environment we were
         // monitoring doesn't exist any more. If this is the case, reset
         // the monitor to the global environment.
         Error error = setEnvironmentName(contextDepth, context, envName);
         if (error)
         {
            persistentState().setActiveEnvironmentName("R_GlobalEnv");
         }
      }
   }
}

Error setEnvironmentMonitoring(const json::JsonRpcRequest& request,
                               json::JsonRpcResponse* pResponse)
{
   bool monitoring = false;
   Error error = json::readParam(request.params, 0, &monitoring);
   if (error)
      return error;

   // save the user's requested monitoring state
   s_monitoring = monitoring;
   persistentState().setEnvironmentMonitoring(s_monitoring);

   return Success();
}

// Remove the given objects from the currently monitored environment.
Error removeObjects(const json::JsonRpcRequest& request,
                    json::JsonRpcResponse* pResponse)
{
   json::Array objectNames;
   Error error = json::readParam(request.params, 0, &objectNames);
   if (error)
      return error;

   error = r::exec::RFunction(".rs.removeObjects")
         .addParam(objectNames)
         .addParam(s_pEnvironmentMonitor->getMonitoredEnvironment())
         .call();
   
   if (error)
      return error;

   return Success();
}

// Remove all the objects from the currently monitored environment.
Error removeAllObjects(const json::JsonRpcRequest& request,
                       json::JsonRpcResponse*)
{
   bool includeHidden = false;
   Error error = json::readParam(request.params, 0, &includeHidden);
   if (error)
      return error;

   error = r::exec::RFunction(".rs.removeAllObjects")
         .addParam(includeHidden)
         .addParam( s_pEnvironmentMonitor->getMonitoredEnvironment())
         .call();
   
   if (error)
      return error;

   return Success();
}

// Return the contents of the given object. Called on-demand by the client when
// the object is large enough that we don't want to get its contents
// immediately (i.e. as part of environmentListAsJson)
Error getObjectContents(const json::JsonRpcRequest& request,
                        json::JsonRpcResponse* pResponse)

{
   std::string objectName;
   Error error = json::readParam(request.params, 0, &objectName);
   if (error)
      return error;
   
   SEXP objContents;
   r::sexp::Protect protect;
   error = r::exec::RFunction(".rs.getObjectContents")
         .addParam(objectName)
         .addParam(s_pEnvironmentMonitor->getMonitoredEnvironment())
          .call(&objContents, &protect);
   
   if (error)
      return error;

   json::Value contents;
   error = r::json::jsonValueFromObject(objContents, &contents);
   if (error)
      return error;

   json::Object result;
   result["contents"] = contents;
   pResponse->setResult(result);
   return Success();
}

// Called by the client to force a re-query of the currently monitored
// context depth and environment.
Error requeryContext(boost::shared_ptr<int> pContextDepth,
                     boost::shared_ptr<LineDebugState> pLineDebugState,
                     boost::shared_ptr<r::context::RCntxt> pCurrentContext,
                     const json::JsonRpcRequest&,
                     json::JsonRpcResponse*)
{
   onConsolePrompt(pContextDepth, pLineDebugState,
                   boost::make_shared<bool>(false), pCurrentContext);
   return Success();
}

// Used by the client to inform the server what language is currently
// being displayed in the Environment pane.
Error environmentSetLanguage(const json::JsonRpcRequest& request,
                             json::JsonRpcResponse*)
{
   std::string language = kEnvironmentLanguageR;
   Error error = json::readParams(request.params, &language);
   if (error)
      LOG_ERROR(error);
   
   s_environmentLanguage = language;
  
   // reset Python module to 'main' after changing language
   if (language == kEnvironmentLanguagePython)
   {
      s_monitoredPythonModule = "__main__";
   }
   
   return Success();
}

// Stores the last "debug: " R output. Used to reconstruct source references
// when unavailable (see simulatedSourceRefsOfContext).
void onConsoleOutput(boost::shared_ptr<LineDebugState> pLineDebugState,
                     boost::shared_ptr<bool> pCapturingDebugOutput,
                     module_context::ConsoleOutputType type,
                     const std::string& output)
{
   if (*pCapturingDebugOutput)
   {
      // stop capturing output if non-normal output occurs
      if (type != module_context::ConsoleOutputNormal)
      {
         *pCapturingDebugOutput = false;
         return;
      }
      pLineDebugState->lastDebugText.append(output);
   }
   else if (type == module_context::ConsoleOutputNormal &&
       output == "debug: ")
   {
      // start capturing debug output when R outputs "debug: "
      pLineDebugState->lastDebugText = "";
      *pCapturingDebugOutput = true;
   }
}


SEXP rs_jumpToFunction(SEXP file, SEXP line, SEXP col, SEXP moveCursor)
{
   json::Object funcLoc;
   FilePath path(r::sexp::safeAsString(file));
   funcLoc["file_name"] = module_context::createAliasedPath(path);
   funcLoc["line_number"] = r::sexp::asInteger(line);
   funcLoc["column_number"] = r::sexp::asInteger(col);
   funcLoc["move_cursor"] = r::sexp::asLogical(moveCursor);
   ClientEvent jumpEvent(client_events::kJumpToFunction, funcLoc);
   module_context::enqueClientEvent(jumpEvent);
   return R_NilValue;
}

} // anonymous namespace

json::Value environmentStateAsJson()
{
   if (s_environmentLanguage == kEnvironmentLanguagePython)
      return pythonEnvironmentStateData(s_monitoredPythonModule);
   
   int contextDepth = 0;
   r::context::getFunctionContext(BROWSER_FUNCTION, &contextDepth);
   
   // If there's no browser on the stack, stay at the top level even if
   // there are functions on the stack--this is not a user debug session.
   if (!r::context::inBrowseContext())
      contextDepth = 0;
   
   return commonEnvironmentStateData(contextDepth, 
         s_monitoring, // include contents if actively monitoring
         nullptr);
}

SEXP rs_isBrowserActive()
{
   r::sexp::Protect protect;
   return r::sexp::create(s_browserActive, &protect);
}

SEXP rs_dumpContexts()
{
   using namespace r::context;
   
   r::sexp::Protect protect;
   r::sexp::ListBuilder contextList(&protect);
   
   for (auto it = RCntxt::begin();
        it != RCntxt::end();
        ++it)
   {
      r::sexp::ListBuilder builder(&protect);
      builder.add("callfun", it->callfun());
      builder.add("callflag", it->callflag());
      builder.add("call", it->call());
      builder.add("srcref", it->srcref());
      builder.add("cloenv", it->cloenv());
      
      SEXP elt = r::sexp::create(builder, &protect);
      contextList.add(elt);
   }
   
   return r::sexp::create(contextList, &protect);
}

bool isSuspendable()
{
   // suppress suspension if any object has a live external pointer; these can't be restored
   return !hasExternalPtr(R_GlobalEnv, false);
}

Error initialize()
{
   // store on the heap so that the destructor is never called (so we
   // don't end up releasing the underlying environment SEXP after
   // R has already shut down / deinitialized)
   s_pEnvironmentMonitor = new EnvironmentMonitor();

   boost::shared_ptr<int> pContextDepth =
         boost::make_shared<int>(0);
   boost::shared_ptr<r::context::RCntxt> pCurrentContext =
         boost::make_shared<r::context::RCntxt>(r::context::globalContext());

   // functions that emit call frames also emit source references; these
   // values capture and supply the currently executing expression emitted by R
   // for the purpose of reconstructing references when none are present.
   boost::shared_ptr<LineDebugState> pLineDebugState =
         boost::make_shared<LineDebugState>();
   boost::shared_ptr<bool> pCapturingDebugOutput =
         boost::make_shared<bool>(false);

   RS_REGISTER_CALL_METHOD(rs_isBrowserActive);
   RS_REGISTER_CALL_METHOD(rs_jumpToFunction);
   RS_REGISTER_CALL_METHOD(rs_hasExternalPointer);
   RS_REGISTER_CALL_METHOD(rs_hasAltrep);
   RS_REGISTER_CALL_METHOD(rs_isAltrep);
   RS_REGISTER_CALL_METHOD(rs_dumpContexts);

   // subscribe to events
   using boost::bind;
   using namespace session::module_context;
   events().onDetectChanges.connect(bind(onDetectChanges, _1));
   events().onConsolePrompt.connect(bind(onConsolePrompt,
                                         pContextDepth,
                                         pLineDebugState,
                                         pCapturingDebugOutput,
                                         pCurrentContext));
   events().onBeforeExecute.connect(onBeforeExecute);
   events().onConsoleOutput.connect(bind(onConsoleOutput,
                                         pLineDebugState,
                                         pCapturingDebugOutput, _1, _2));

   json::JsonRpcFunction listEnv =
         boost::bind(listEnvironment, pContextDepth, _1, _2);
   json::JsonRpcFunction setCtxDepth =
         boost::bind(setContextDepth, pContextDepth, pLineDebugState,
                     _1, _2);
   json::JsonRpcFunction getEnv =
         boost::bind(getEnvironmentState, pContextDepth, pLineDebugState,
                     _1, _2);
   json::JsonRpcFunction getEnvNames =
         boost::bind(getEnvironmentNames, pContextDepth, pCurrentContext,
                     _1, _2);
   json::JsonRpcFunction setEnvName =
         boost::bind(setEnvironment, pContextDepth, pCurrentContext,
                     _1, _2);
   json::JsonRpcFunction requeryCtx =
         boost::bind(requeryContext, pContextDepth, pLineDebugState,
                     pCurrentContext, _1, _2);

   initEnvironmentMonitoring();

   ExecBlock initBlock;
   initBlock.addFunctions()
      (bind(registerRBrowseFileHandler, handleRBrowseEnv))
      (bind(registerRpcMethod, "list_environment", listEnv))
      (bind(registerRpcMethod, "set_context_depth", setCtxDepth))
      (bind(registerRpcMethod, "set_environment", setEnvName))
      (bind(registerRpcMethod, "set_environment_frame", setEnvironmentFrame))
      (bind(registerRpcMethod, "get_environment_names", getEnvNames))
      (bind(registerRpcMethod, "remove_objects", removeObjects))
      (bind(registerRpcMethod, "remove_all_objects", removeAllObjects))
      (bind(registerRpcMethod, "get_environment_state", getEnv))
      (bind(registerRpcMethod, "get_object_contents", getObjectContents))
      (bind(registerRpcMethod, "requery_context", requeryCtx))
      (bind(registerRpcMethod, "environment_set_language", environmentSetLanguage))
      (bind(registerRpcMethod, "set_environment_monitoring", setEnvironmentMonitoring))
      (bind(sourceModuleRFile, "SessionEnvironment.R"));

   return initBlock.execute();
}
   
} // namespace environment
} // namespace modules
} // namespace session
} // namespace rstudio

