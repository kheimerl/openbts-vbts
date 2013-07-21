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

#include <Logger.h>
#include <fcntl.h>
#include <unistd.h>
#include "PARPCClient.h"
#include "config.h"

using namespace std;

#define SERVER_LOC "http://localhost:8080/RPC2"
#define ON_METHOD "on"
#define OFF_METHOD "off"

#define WAIT_TIME 30

xmlrpc_c::value_string const reason("OpenBTS Activity");

void run_rpc(PARPCClient* rpc){
    rpc->driveLoop();
}

/*public*/
void PARPCClient::driveLoop()
{
    while (running){
	if (changed){
	    if (cur_on){
		pa_on();
	    }
	    else{
		pa_off();
	    }
	    changed = false;
	}
	usleep(10000);
	//mDriveThread.yield();
    }
}

void PARPCClient::start(){
    running = true;
    mDriveThread.start((void *(*)(void*))run_rpc,this);
    LOG(INFO) << "PA Client Starting";
    pa_on();
}

void PARPCClient::stop()
{
    running = false;
}

/* we don't need to lock on these
   as there's just one thread reading them */
void PARPCClient::on()
{
    cur_on = true;
    changed = true;
}

void PARPCClient::off()
{
    cur_on = false;
    changed = true;
}


/*private*/
/* we dont need to lock any of these, as only one thread is handling
   any of the changes to time */
void PARPCClient::pa_on(){
    LOG(INFO) << "PA On from client";
    if (time(NULL) > last_update + WAIT_TIME){
	xmlrpc_c::value result;
	//xmlrpc_c::paramList params;
	//params.add(reason);
	client.call(SERVER_LOC, ON_METHOD, "", &result);
	last_update = time(NULL);
    }
}

void PARPCClient::pa_off(){
    LOG(INFO) << "PA Off from client";
    xmlrpc_c::value result;
    //xmlrpc_c::paramList params;
    //params.add(reason);
    client.call(SERVER_LOC, OFF_METHOD, "", &result);
}
