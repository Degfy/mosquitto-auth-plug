/*
 * Copyright (c) 2013 Jan-Piet Mens <jpmens()gmail.com>
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of mosquitto nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <mosquitto.h>
#include <mosquitto_plugin.h>
#include <fnmatch.h>
#include "log.h"
#include "hash.h"
#include "backends.h"

#ifdef BE_CDB
# include "be-cdb.h"
#endif
#ifdef BE_MYSQL
# include "be-mysql.h"
#endif
#ifdef BE_SQLITE
# include "be-sqlite.h"
#endif

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

#define NBACKENDS	(4)


struct backend_p {
	void *conf;			/* Handle to backend */
	char *name;
	f_kill *kill;
	f_getuser *getuser;
	f_superuser *superuser;
	f_aclcheck *aclcheck;
};

struct userdata {
	struct backend *be;		/* Connection to back-end */

	struct backend_p **be_list;



	// redisContext *redis;
	char *host;
	int port;
	char *usernameprefix;		/* e.g. "u:" */
	char *topicprefix;
	char *superusers;		/* fnmatch-style glob */
	char *dbpath;
	char *sql_userquery;
	char *sql_aclquery;
};

int pbkdf2_check(char *password, char *hash);

int mosquitto_auth_plugin_version(void)
{
	fprintf(stderr, "*** auth-plug: backend=%s\n", TOSTRING(BACKEND));
	return MOSQ_AUTH_PLUGIN_VERSION;
}

int mosquitto_auth_plugin_init(void **userdata, struct mosquitto_auth_opt *auth_opts, int auth_opt_count)
{
	int i;
	char *backends = NULL, *p, *q;
	struct mosquitto_auth_opt *o;
	struct userdata *ud;
	int ret = MOSQ_ERR_SUCCESS;
	int nord;
	struct backend_p **bep;
#ifdef BE_CDB
	char *cdbpath = NULL;
#endif /* CDB */

#ifdef BE_MYSQL
	char *host = NULL;
	int port  = 0;
	char *dbname = NULL;
	char *user = NULL;
	char *pass = NULL;
	char *userquery = NULL;
	char *superquery = NULL;
	char *aclquery = NULL;
#endif

	*userdata = (struct userdata *)malloc(sizeof(struct userdata));
	if (*userdata == NULL) {
		perror("allocting userdata");
		return MOSQ_ERR_UNKNOWN;
	}

	ud = *userdata;
	ud->be			= 0;
	ud->dbpath		= NULL;  /* path name for SQLite & CDB */
	ud->sql_userquery	= NULL;
	ud->sql_aclquery	= NULL;
	ud->usernameprefix	= NULL;
	ud->topicprefix		= NULL;
	ud->superusers		= NULL;
	ud->host		= NULL;
	ud->port		= 6379;

	/*
	 * Shove all options Mosquitto gives the plugin into a hash,
	 * and let the back-ends figure out if they have all they
	 * need upon init()
	 */

	for (i = 0, o = auth_opts; i < auth_opt_count; i++, o++) {
		_log(LOG_DEBUG, "AuthOptions: key=%s, val=%s", o->key, o->value);

		p_add(o->key, o->value);

		if (!strcmp(o->key, "superusers"))
			ud->superusers = strdup(o->value);
		if (!strcmp(o->key, "topic_prefix"))
			ud->topicprefix = strdup(o->value);
	}

	/*
	 * Set up back-ends, and tell them to initialize themselves.
	 */


	backends = p_stab("backends");
	if (backends == NULL) {
		_fatal("No backends configured.");
	}

        p = strdup(backends);

        printf("** Configured order: %s\n", p);

	ud->be_list = (struct backend_p **)malloc((sizeof (struct backend_p *)) * (NBACKENDS + 1));

	bep = ud->be_list;
        for (nord = 0, q = strsep(&p, ","); q && *q && (nord < NBACKENDS); q = strsep(&p, ",")) {
                int found = 0;

		if (!strcmp(q, "mysql")) {
			puts("---> MYSQL");
			be_add("mysql");
			*bep = (struct backend_p *)malloc(sizeof(struct backend_p));
			memset(*bep, 0, sizeof(struct backend_p));
			(*bep)->name = strdup("mysql");
			(*bep)->conf = be_mysql_init();
			if ((*bep)->conf == NULL) {
				_fatal("%s init returns NULL", q);
			}
			(*bep)->kill =  be_mysql_destroy;
			(*bep)->getuser =  be_mysql_getuser;
			(*bep)->superuser =  be_mysql_superuser;
			(*bep)->aclcheck =  be_mysql_aclcheck;
			found = 1;
		}

		if (!strcmp(q, "cdb")) {
			puts("---> CDB");
			be_add("cdb");
			*bep = (struct backend_p *)malloc(sizeof(struct backend_p));
			memset(*bep, 0, sizeof(struct backend_p));
			(*bep)->name = strdup("cdb");
			(*bep)->conf = be_cdb_init();
			if ((*bep)->conf == NULL) {
				_fatal("%s init returns NULL", q);
			}
			(*bep)->kill =  be_cdb_destroy;
			(*bep)->getuser =  be_cdb_getuser;
			(*bep)->superuser =  be_cdb_superuser;
			(*bep)->aclcheck =  be_cdb_aclcheck;
			found = 1;
		}

                if (!found) {
                        _fatal("ERROR: configured back-end `%s' doesn't exist", q);
                }

		ud->be_list[++nord] = NULL;
		bep++;
        }

        free(p);

	be_dump();


#ifdef BE_CDB
	if (cdbpath == NULL) {
		fprintf(stderr, "No cdbpath specified for CDB back-end\n");
		return (MOSQ_ERR_UNKNOWN);
	}
	// ud->be = be_cdb_init(cdbpath);
#endif

#ifdef BE_MYSQL
	if (host == NULL)
		host = strdup("localhost");

	if (userquery == NULL) {
		fprintf(stderr, "Userquery is mandatory for back-end MySQL\n");
		return (MOSQ_ERR_UNKNOWN);
	}

	ud->be = be_mysql_init(host, port, user, pass, dbname,
				userquery, superquery, aclquery); 
#endif

#ifdef BE_SQLITE
	if (dbpath == NULL) {
		fprintf(stderr, "No dbpath specified for sqlite back-end\n");
		return (MOSQ_ERR_UNKNOWN);
	}

	if (ud->sql_userquery == NULL) {
		fprintf(stderr, "No SQL query specified for sqlite back-end\n");
		return (MOSQ_ERR_UNKNOWN);
	}

	ud->be = be_sqlite_init(ud->dbpath, ud->sql_userquery);
	goto out;
#endif /* SQLITE */

#ifdef BE_REDIS
	if (ud->host == NULL)
		ud->host = strdup("localhost");

	//ud->redis = redis_init(ud->host, ud->port);
	//if (ud->redis == NULL) {
		//fprintf(stderr, "Cannot connect to Redis on %s:%d\n", ud->host, ud->port);
		//ret = MOSQ_ERR_UNKNOWN;
	//}
	goto out;
#endif /* REDIS */

	return (ret);
}

int mosquitto_auth_plugin_cleanup(void *userdata, struct mosquitto_auth_opt *auth_opts, int auth_opt_count)
{
	// struct userdata *ud = (struct userdata *)userdata;
	// struct backend *be = ud->be;

	/* FIXME: fee other elements */

	return MOSQ_ERR_SUCCESS;
}

int mosquitto_auth_security_init(void *userdata, struct mosquitto_auth_opt *auth_opts, int auth_opt_count, bool reload)
{
	return MOSQ_ERR_SUCCESS;
}

int mosquitto_auth_security_cleanup(void *userdata, struct mosquitto_auth_opt *auth_opts, int auth_opt_count, bool reload)
{
	return MOSQ_ERR_SUCCESS;
}



int mosquitto_auth_unpwd_check(void *userdata, const char *username, const char *password)
{
	struct userdata *ud = (struct userdata *)userdata;
	struct backend_p **bep;
	char *phash = NULL;
	int match, authorized = FALSE;

	if (!username || !*username || !password || !*password)
		return MOSQ_ERR_AUTH;


	for (bep = ud->be_list; bep && *bep; bep++) {
		struct backend_p *b = *bep;


		_log(DEBUG, "... %s: getuser(%s, %s)", b->name, username, password);

		phash = b->getuser(b->conf, username);
		if (phash != NULL) {
			match = pbkdf2_check((char *)password, phash);
			_log(LOG_DEBUG, "%s: unpwd_check: for user=%s, got: %s", b->name, username, phash);
			_log(LOG_DEBUG, "%s: unpwd_check: PBKDF2 match == %d", b->name, match);
			if (match == 1) {
				authorized = TRUE;
				break;
			}
		}
	}
	
	if (phash == NULL) {
		return MOSQ_ERR_AUTH;
	}

	free(phash);
	return (authorized) ? MOSQ_ERR_SUCCESS : MOSQ_ERR_AUTH;
}

int mosquitto_auth_acl_check(void *userdata, const char *clientid, const char *username, const char *topic, int access)
{
	struct userdata *ud = (struct userdata *)userdata;
	char *tname;
	int tlen, match = 0;

	_log(LOG_DEBUG, "!!!! acl_check u=%s, t=%s, a=%d",
		(username) ? username : "NIL",
		(topic) ? topic : "NIL",
		access);

	if (!username || !*username)
		return MOSQ_ERR_ACL_DENIED;

	/* Check for usernames exempt from ACL checking, first */

	if (ud->superusers) {
		if (fnmatch(ud->superusers, username, 0) == 0) {
			_log(LOG_DEBUG, "** !!! %s is superuser", username);
			return MOSQ_ERR_SUCCESS;
		}
	}

#ifdef BE_CDB
	// match = be_cdb_access(ud->be, username, (char *)topic);
#endif

#ifdef BE_MYSQL
	match = be_mysql_superuser(ud->be, username) ||
		be_mysql_aclcheck(ud->be, username, topic, access);
#endif
	fprintf(stderr, "** !!! %s PERMITTED for %s\n", username, topic);
	if (match)
		return (match);

	if (ud->topicprefix) {
		char *s, *t;
		int n;
		bool bf;

		/* Count number of '%' in topicprefix */
		for (n = 0, s = ud->topicprefix; s && *s; s++) {
			if (*s == '%')
				++n;
		}

		tlen = strlen(ud->topicprefix) + (strlen(username) * n) + 1;
		tname = malloc(tlen);

		/* Create new topic in tname with all '%' replaced by username */
		*tname = 0;
		for (t = tname, s = ud->topicprefix; s && *s; ) {
			if (*s != '%') {
				*t++ = *s++;
				*t = 0;
			} else {
				strcat(tname, username);
				t = tname + strlen(tname);
				s++;
			}
		}

		if (strcmp(topic, tname) == 0)
			match = 1;

		/*
		 * Check for MQTT wildcard matches in the newly constructed
		 * topic name, and OR that into matches, allowing if allowed.
		 */

		mosquitto_topic_matches_sub(tname, topic, &bf);
		match |= bf;

		free(tname);
	}

	_log(LOG_NOTICE, "** ACL match == %d", match);

	if (match == 1) {
		return MOSQ_ERR_SUCCESS;
	} 
	return MOSQ_ERR_ACL_DENIED;
}


int mosquitto_auth_psk_key_get(void *userdata, const char *hint, const char *identity, char *key, int max_key_len)
{
	return MOSQ_ERR_AUTH;
}
