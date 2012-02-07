/* 
 * Written by Kurtis Heimerl <kheimerl@cs.berkeley.edu>
 *
 * Copyright (c) 2011 Kurtis Heimerl
 *
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 
 * Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 * 
 * Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 * 
 * Neither the name of the project's author nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 * TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <stdio.h>
#include <time.h>
#include <Logger.h>
#include "PAController.h"

//XMLRPC stuff
#include <cassert>
#include <stdexcept>
#include <iostream>
#include <unistd.h>

using namespace std;

//I hate C++ -kurtis

#define RPC_PORT 8080
#define RPC_LOG_LOC "/tmp/xmlrpc.log"

static bool pa_on = false;
>>>>>>> 6712a11... New RPC server hosted by the transceiver

//the "turn the PA on method"
class on_method : public xmlrpc_c::method {
public:
  on_method() {
    // signature and help strings are documentation -- the client
    // can query this information with a system.methodSignature and
    // system.methodHelp RPC.
    this->_signature = "n:";
    this->_help = "This method turns the PA on";
  }
  void
  execute(xmlrpc_c::paramList const& paramList,
	  xmlrpc_c::value *   const  retvalP) {
    LOG(ALERT) << "Kurtis: Network ON";
    pa_on = true;
    *retvalP = xmlrpc_c::value_nil();
  }
};

//the "turn the PA on method"
class off_method : public xmlrpc_c::method {
public:
  off_method() {
    this->_signature = "n:"; 
    this->_help = "This method turns the PA off";
  }
  void
  execute(xmlrpc_c::paramList const& paramList,
	  xmlrpc_c::value *   const  retvalP) {
    LOG(ALERT) << "Kurtis: Network OFF";
    pa_on = false;
    *retvalP = xmlrpc_c::value_nil();
  }
};

//the "turn the PA on method"
class status_method : public xmlrpc_c::method {
public:
  status_method() {
    // signature and help strings are documentation -- the client
    // can query this information with a system.methodSignature and
    // system.methodHelp RPC.
    this->_signature = "b:";
    this->_help = "This method returns the PA status";
  }
  void
  execute(xmlrpc_c::paramList const& paramList,
	  xmlrpc_c::value *   const  retvalP) {
    *retvalP = xmlrpc_c::value_boolean(pa_on);
  }
};

PAController::PAController()
{
  registry = new xmlrpc_c::registry();

  xmlrpc_c::methodPtr const onMethod(new on_method);
  xmlrpc_c::methodPtr const offMethod(new off_method);
  xmlrpc_c::methodPtr const statusMethod(new status_method);

  registry->addMethod("on", onMethod);
  registry->addMethod("off", offMethod);
  registry->addMethod("status", statusMethod);

  RPCServer = new xmlrpc_c::serverAbyss(*registry,
					RPC_PORT,
					RPC_LOG_LOC
					);
}

PAController::~PAController()
{
  //should call the deconstructor and close cleanly... right?
  delete RPCServer;
  delete registry;
}

void PAController::run()
{
  RPCServer->run();
}

void PAController::on()
{
  pa_on = true;
  LOG(ALERT) << "Kurtis: Local ON";
}

void PAController::off()
{
  pa_on = false;
  LOG(ALERT) << "Kurtis: Local OFF";
}

bool PAController::state()
{
  return pa_on;
}

/* non-member functions */
void runController(PAController* cont)
{
  Thread RPCThread;
  RPCThread.start((void*(*)(void*)) &PAController::run, cont);
}
