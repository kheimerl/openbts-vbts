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
#include <fcntl.h>
#include <string.h>
#include <Configuration.h>
#include "PAController.h"
#include "config.h"

#if defined USE_UHD || defined USE_USRP1
#define DONT_USE_SERIAL 1
#endif

extern ConfigurationTable gConfig;

using namespace std;

//I hate C++ -kurtis

#define DEFAULT_START_TIME "00:00"
#define DEFAULT_END_TIME "00:00"
#define TIME_FORMAT "%H:%M"

static bool pa_on = false;
static Mutex pa_lock;
static time_t last_update = NULL;

static struct tm start_tm;
static struct tm end_tm;

#ifndef DONT_USE_SERIAL
static int fd1;
static string on_cmd;
static string off_cmd;
#endif

//hack for now, as I want one source file for both RAD1 and UHD/USRP1

/* assumes you hold the lock */
static void actual_pa_off(string reason){
    LOG(ALERT) << "PA Off:" << pa_on << ":" << reason;
    pa_on = false;
#ifndef DONT_USE_SERIAL
    fcntl(fd1,F_SETFL,0);
    write(fd1,off_cmd.c_str(), off_cmd.length());
    write(fd1,off_cmd.c_str(), off_cmd.length());
#endif
}

static void turn_pa_on(bool resetTime, string reason){
    ScopedLock lock (pa_lock);
    //don't think I need to garbage collect, it's just an int
    if (!pa_on || resetTime){
	LOG(ALERT) << "PA On:" << pa_on << ":" << reason;
	last_update = time(NULL);
	pa_on = true;
#ifndef DONT_USE_SERIAL
	fcntl(fd1,F_SETFL,0);
	write(fd1,on_cmd.c_str(), on_cmd.length());
	write(fd1,on_cmd.c_str(), on_cmd.length());
#endif
    }
}

static void turn_pa_off(string reason){
  ScopedLock lock (pa_lock);
  actual_pa_off(reason);
}

/* key point: this is being called all the time
   by the transceiver, allowing it to be updated
   almost immediately after time stamp ends */
bool update_pa(){

    //first check for time
    time_t rawtime;
    struct tm * timeinfo; //this is statically defined in library, no need to free
    time(&rawtime);
    timeinfo = localtime(&rawtime);
    //exit if we're after start time and before end time
    if (((timeinfo->tm_hour > start_tm.tm_hour) ||
	 (timeinfo->tm_hour == start_tm.tm_hour && timeinfo->tm_min > start_tm.tm_min)) &&
	((timeinfo->tm_hour < end_tm.tm_hour) || 
	 (timeinfo->tm_hour == end_tm.tm_hour &&  timeinfo->tm_min < end_tm.tm_min))){
	turn_pa_on(false, "Time of Day");
	return pa_on;
    }

    //otherwise see if we should turn the PA off
    int pa_timeout = gConfig.getNum("VBTS.PA.Timeout", 5 * 60);
    ScopedLock lock (pa_lock);
    if (pa_on && last_update && 
	rawtime > pa_timeout + last_update){
	actual_pa_off("Timeout");
	LOG(ALERT) << "Timeout:" << pa_timeout;
    }
    return pa_on;
}

//the "turn the PA on method"
class on_method : public xmlrpc_c::method {
public:
    on_method() {
	this->_signature = "n:";
	this->_help = "This method turns the PA on";
    }
    void
    execute(xmlrpc_c::paramList const& paramList,
	    xmlrpc_c::value *   const  retvalP) {
	turn_pa_on(true, "None");
	*retvalP = xmlrpc_c::value_nil();
    }
};

//the "turn the PA on method"
class on_method_reason : public xmlrpc_c::method {
public:
    on_method_reason() {
	this->_signature = "n:s";
	this->_help = "This method turns the PA on and records the reason";
    }
    void
    execute(xmlrpc_c::paramList const& paramList,
	    xmlrpc_c::value *   const  retvalP) {
	turn_pa_on(true, paramList.getString(0));
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
	turn_pa_off("None");
	*retvalP = xmlrpc_c::value_nil();
    }
};

//the "turn the PA on method"
class off_method_reason : public xmlrpc_c::method {
public:
    off_method_reason() {
	this->_signature = "n:s"; 
	this->_help = "This method turns the PA off and records the reason";
    }
    void
    execute(xmlrpc_c::paramList const& paramList,
	    xmlrpc_c::value *   const  retvalP) {
	turn_pa_off(paramList.getString(0));
	*retvalP = xmlrpc_c::value_nil();
    }
};

class status_method : public xmlrpc_c::method {
public:
    status_method() {
	this->_signature = "b:";
	this->_help = "This method returns the PA status";
    }
    void
    execute(xmlrpc_c::paramList const& paramList,
	    xmlrpc_c::value *   const  retvalP) {
	*retvalP = xmlrpc_c::value_boolean(update_pa());
    }
};

/* please instantiate me only once -kurtis*/
PAController::PAController()
{
    
    registry = new xmlrpc_c::registry();
    
    xmlrpc_c::methodPtr const onMethod(new on_method);
    xmlrpc_c::methodPtr const onMethodReason(new on_method_reason);
    xmlrpc_c::methodPtr const offMethod(new off_method);
    xmlrpc_c::methodPtr const offMethodReason(new off_method_reason);
    xmlrpc_c::methodPtr const statusMethod(new status_method);
    
    registry->addMethod("on", onMethod);
    registry->addMethod("onWithReason", onMethodReason);
    registry->addMethod("off", offMethod);
    registry->addMethod("offWithReason", offMethodReason);
    registry->addMethod("status", statusMethod);
    
    long rpc_port = gConfig.getNum("VBTS.PA.RPCPort", 8080);
    string rpc_log = gConfig.getStr("VBTS.PA.RPCLogLoc", "/tmp/xmlrpc.log");
    
    RPCServer = new xmlrpc_c::serverAbyss(*registry,
					  rpc_port,
					  rpc_log
	);

#ifndef DONT_USE_SERIAL
    string serial_loc = gConfig.getStr("VBTS.PA.SerialLoc", "/dev/ttyACM0");
    fd1 = open (serial_loc.c_str(), O_RDWR | O_NOCTTY | O_NDELAY);

    on_cmd = gConfig.getStr("VBTS.PA.OnCommand", "O0=1\r");
    off_cmd = gConfig.getStr("VBTS.PA.OffCommand", "O0=0\r");
#endif

    string start_time = gConfig.getStr("VBTS.PA.StartTime", DEFAULT_START_TIME);
    string end_time = gConfig.getStr("VBTS.PA.EndTime", DEFAULT_END_TIME);

    if (strptime(start_time.c_str(), TIME_FORMAT, &start_tm) ==  NULL){
	LOG(ALERT) << "MALFORMED START TIME";
	strptime(DEFAULT_START_TIME, TIME_FORMAT, &start_tm);
    }
    
    if (strptime(end_time.c_str(), TIME_FORMAT, &end_tm) ==  NULL){
	LOG(ALERT) << "MALFORMED END TIME";
	strptime(DEFAULT_END_TIME, TIME_FORMAT, &end_tm);
    }
    
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

void PAController::on(string reason)
{
    turn_pa_on(false, reason);
}

void PAController::off(string reason)
{
    turn_pa_off(reason);
}

bool PAController::state()
{
    return update_pa();
}

/* non-member functions */
void runController(PAController* cont)
{
    Thread RPCThread;
    RPCThread.start((void*(*)(void*)) &PAController::run, cont);
    cont->on("Starting");
}
