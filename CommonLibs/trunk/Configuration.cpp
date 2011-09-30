/*
* Copyright 2008, 2009, 2010 Free Software Foundation, Inc.
* Copyright 2010 Kestrel Signal Processing, Inc.
*
* This software is distributed under multiple licenses; see the COPYING file in the main directory for licensing information for this specific distribuion.
*
* This use of this software may be subject to additional restrictions.
* See the LEGAL file in the main directory for details.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

*/


#include "Configuration.h"
#include <fstream>
#include <iostream>
#include <string.h>

using namespace std;


static const char* createConfigTable = {
	"CREATE TABLE IF NOT EXISTS CONFIG ("
		"KEYSTRING TEXT UNIQUE NOT NULL, "
		"VALUESTRING TEXT, "
		"STATIC INTEGER DEFAULT 0, "
		"OPTIONAL INTEGER DEFAULT 0"
	")"
};


ConfigurationTable::ConfigurationTable(const char* filename)
{
	// Connect to the database.
	int rc = sqlite3_open(filename,&mDB);
	if (rc) {
		cerr << "Cannot open configuration database: " << sqlite3_errmsg(mDB);
		sqlite3_close(mDB);
		mDB = NULL;
		return;
	}
	// Create the table, if needed.
	if (!sqlite3_command(mDB,createConfigTable)) {
		cerr << "Cannot create configuration table:" << sqlite3_errmsg(mDB);
	}
}



bool ConfigurationTable::defines(const string& key)
{
	assert(mDB);
	mLock.lock();
	// Check the cache.
	ConfigurationMap::const_iterator where = mCache.find(key);
	if (where!=mCache.end()) {
		bool defined = where->second.defined();
		mLock.unlock();
		return defined;
	}
	// Check the database.
	char *value = NULL;
	bool defined = sqlite3_single_lookup(mDB,"CONFIG",
			"KEYSTRING",key.c_str(),"VALUESTRING",value);
	// Cache the result.
	if (value) {
		mCache[key] = ConfigurationRecord(value);
		free(value);
	} else {
		mCache[key] = ConfigurationRecord(defined);
	}
	// Done.
	mLock.unlock();
	return defined;
}


const ConfigurationRecord& ConfigurationTable::lookup(const string& key)
{
	assert(mDB);
	// We assume the called holds mLock.
	// So it is OK to return a reference into the cache.

	// Check the cache.
	// This is cheap.
	ConfigurationMap::const_iterator where = mCache.find(key);
	if (where!=mCache.end()) {
		if (where->second.defined()) return where->second;
		// Unlock the mutex before throwing the exception.
		mLock.unlock();
		throw ConfigurationTableKeyNotFound(key);
	}

	// Check the database.
	// This is more expensive.
	char *value = NULL;
	bool defined = sqlite3_single_lookup(mDB,"CONFIG",
			"KEYSTRING",key.c_str(),"VALUESTRING",value);
	if (!defined) {
		// Unlock the mutex before throwing the exception.
		mLock.unlock();
		throw ConfigurationTableKeyNotFound(key);
	}
	// Cache the result.
	if (value) {
		mCache[key] = ConfigurationRecord(value);
		free(value);
	} else {
		mCache[key] = ConfigurationRecord(true);
	}
	// Leave mLock locked.  The caller holds it still.
	return mCache[key];
}



bool ConfigurationTable::isStatic(const string& key) const
{
	assert(mDB);
	unsigned stat;
	bool success = sqlite3_single_lookup(mDB,"CONFIG","KEYSTRING",key.c_str(),"STATIC",stat);
	if (success) return (bool)stat;
	return false;
}

bool ConfigurationTable::isRequired(const string& key) const
{
	assert(mDB);
	unsigned optional;
	bool success = sqlite3_single_lookup(mDB,"CONFIG","KEYSTRING",key.c_str(),"OPTIONAL",optional);
	if (success) return !((bool)optional);
	return false;
}




string ConfigurationTable::getStr(const string& key)
{
	mLock.lock();
	const ConfigurationRecord& rec = lookup(key);
	string retVal = rec.value();
	mLock.unlock();
	return retVal;
}

long ConfigurationTable::getNum(const string& key)
{
	mLock.lock();
	const ConfigurationRecord& rec = lookup(key);
	long retVal = rec.number();
	mLock.unlock();
	return retVal;
}



std::vector<unsigned> ConfigurationTable::getVector(const string& key)
{
	// Look up the string.
	mLock.lock();
	const ConfigurationRecord& rec = lookup(key);
	char* line = strdup(rec.value().c_str());
	mLock.unlock();
	// Parse the string.
	std::vector<unsigned> retVal;
	char *lp=line;
	while (lp) {
		retVal.push_back(strtol(lp,NULL,10));
		strsep(&lp," ");
	}
	free(line);
	return retVal;
}


bool ConfigurationTable::unset(const string& key)
{
	assert(mDB);
	// Can it be unset?
	//if (isStatic(key)) return false;
	if (isRequired(key)) return false;
	// Clear the cache entry and the database.
	mLock.lock();
	ConfigurationMap::iterator where = mCache.find(key);
	if (where!=mCache.end()) mCache.erase(where);
	string cmd = "DELETE FROM CONFIG WHERE KEYSTRING == \"" + key + "\"";
	bool success = sqlite3_command(mDB,cmd.c_str());
	mLock.unlock();
	return success;
}


void ConfigurationTable::find(const string& pat, ostream& os) const
{
	// Prepare the statement.
	string cmd = "SELECT KEYSTRING,VALUESTRING FROM CONFIG WHERE KEYSTRING LIKE \"%" + pat + "%\"";
	sqlite3_stmt *stmt;
	if (sqlite3_prepare_statement(mDB,&stmt,cmd.c_str())) return;
	// Read the result.
	int src = sqlite3_run_query(mDB,stmt);
	while (src==SQLITE_ROW) {
		os << sqlite3_column_text(stmt,0) << " ";
		const char* value = (const char*)sqlite3_column_text(stmt,1);
		if (value) os << value;
		os << endl;
		src = sqlite3_run_query(mDB,stmt);
	}
	sqlite3_finalize(stmt);
}


bool ConfigurationTable::set(const string& key, const string& value)
{
	assert(mDB);
	//if (isStatic(key)) return false;
	mLock.lock();
	// Is it there already?
	bool exists = defines(key);
	string cmd;
	if (exists) cmd = "UPDATE CONFIG SET VALUESTRING=\""+value+"\" WHERE KEYSTRING==\""+key+"\"";
	else cmd = "INSERT INTO CONFIG (KEYSTRING,VALUESTRING,OPTIONAL) VALUES (\"" + key + "\",\"" + value + "\",1)";
	bool success = sqlite3_command(mDB,cmd.c_str());
	if (success) mCache[key] = ConfigurationRecord(value);
	mLock.unlock();
	return success;
}

bool ConfigurationTable::set(const string& key, long value)
{
	char buffer[30];
	sprintf(buffer,"%ld",value);
	return set(key,buffer);
}


bool ConfigurationTable::set(const string& key)
{
	assert(mDB);
	//if (isStatic(key)) return false;
	mLock.lock();
	string cmd = "INSERT INTO CONFIG (KEYSTRING) VALUES (\"" + key + "\")";
	bool success = sqlite3_command(mDB,cmd.c_str());
	if (success) mCache[key] = ConfigurationRecord(true);
	mLock.unlock();
	return success;
}


void ConfigurationTable::purge()
{
	mLock.lock();
	ConfigurationMap::iterator mp = mCache.begin();
	while (mp != mCache.end()) {
		ConfigurationMap::iterator prev = mp;
		mp++;
		mCache.erase(prev);
	}
	mLock.unlock();
}


void ConfigurationTable::setUpdateHook(void(*func)(void *,int ,char const *,char const *,sqlite3_int64))
{
	assert(mDB);
	sqlite3_update_hook(mDB,func,NULL);
}



void HashString::computeHash()
{
	// FIXME -- This needs to be a proper hash function.
	mHash = 0;
	for (unsigned i=0; i<size(); i++) {
		mHash = mHash ^ (mHash >> 32);
		mHash = mHash*127 + this->operator[](i);
	}
}



// vim: ts=4 sw=4
