/*
 *
 * (C) 2013 - ntop.org
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 */

#include "ntop_includes.h"

#define USE_LUA
#include "./third-party/mongoose/mongoose.c"
#undef USE_LUA

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
};

static HTTPserver *httpserver;

bool enable_users_login = true;

/* ****************************************** */

/*
 * Send error message back to a client.
 */
int send_error(struct mg_connection *conn, int status, const char *reason, const char *fmt, ...) {
  char		buf[BUFSIZ];
  va_list		ap;
  int		len;

  conn->status_code = status;

  (void) mg_printf(conn,
		   "HTTP/1.1 %d %s\r\n"
		   "Content-Type: text/html\r\n"
		   "Connection: close\r\n"
		   "\r\n", status, reason);

  /* Errors 1xx, 204 and 304 MUST NOT send a body */
  if (status > 199 && status != 204 && status != 304) {
    conn->num_bytes_sent = 0;
    va_start(ap, fmt);
    len = mg_vsnprintf(conn, buf, sizeof(buf), fmt, ap);
    va_end(ap);
    conn->num_bytes_sent += mg_write(conn, buf, len);
    cry(conn, "%s", buf);
  }

  return(1);
}

/* ****************************************** */

#ifdef HAVE_SSL
static void redirect_to_ssl(struct mg_connection *conn,
                            const struct mg_request_info *request_info) {
  const char *p, *host = mg_get_header(conn, "Host");
  u_int16_t port = ntop->get_HTTPserver()->get_port();

  if (host != NULL && (p = strchr(host, ':')) != NULL) {
    mg_printf(conn, "HTTP/1.1 302 Found\r\n"
              "Location: https://%.*s:%u/%s\r\n\r\n",
              (int) (p - host), host, port+1, request_info->uri);
  } else {
    mg_printf(conn, "%s", "HTTP/1.1 500 Error\r\n\r\nHost: header is not set");
  }
}
#endif

/* ****************************************** */

// Generate session ID. buf must be 33 bytes in size.
// Note that it is easy to steal session cookies by sniffing traffic.
// This is why all communication must be SSL-ed.
static void generate_session_id(char *buf, const char *random, const char *user) {
  mg_md5(buf, random, user, NULL);
}

/* ****************************************** */

// Return 1 if request is authorized, 0 otherwise.
static int is_authorized(const struct mg_connection *conn,
                         const struct mg_request_info *request_info) {
  char key[64], user[32];
  char session_id[33], username[33];

  // Always authorize accesses to login page and to authorize URI
  if (!strcmp(request_info->uri, LOGIN_URL) ||
      !strcmp(request_info->uri, AUTHORIZE_URL)) {
    return 1;
  }

  mg_get_cookie(conn, "session", session_id, sizeof(session_id));
  if(session_id[0] == '\0') return(0);

  mg_get_cookie(conn, "user", username, sizeof(username));
  
  // ntop->getTrace()->traceEvent(TRACE_WARNING, "[HTTP] Received session %s/%s", session_id, username);

  snprintf(key, sizeof(key), "sessions.%s", session_id);
  if((ntop->getRedis()->get(key, user, sizeof(user)) < 0)
     || strcmp(user, username) /* Users don't match */) {
    ntop->getTrace()->traceEvent(TRACE_INFO, "[HTTP] Session %s/%s is expired or empty user", 
				 session_id, username);
    return(0);
  } else {
    ntop->getRedis()->expire(key, HTTP_SESSION_DURATION); /* Extend session */
    ntop->getTrace()->traceEvent(TRACE_INFO, "[HTTP] Session %s is OK: extended for %u sec", 
				 session_id, HTTP_SESSION_DURATION);
    return(1);
  }
}

/* ****************************************** */

// Redirect user to the login form. In the cookie, store the original URL
// we came from, so that after the authorization we could redirect back.
static void redirect_to_login(struct mg_connection *conn,
                              const struct mg_request_info *request_info) {

  char session_id[33];

  mg_get_cookie(conn, "session", session_id, sizeof(session_id));
  ntop->getTrace()->traceEvent(TRACE_INFO, "[HTTP] %s(%s)", __FUNCTION__, session_id);
  
  mg_printf(conn, "HTTP/1.1 302 Found\r\n"
	    "Set-Cookie: session=%s; path=/; expires=Thu, 01-Jan-1970 00:00:01 GMT; max-age=0; HttpOnly\r\n"  // Session ID
	    "Location: %s\r\n\r\n",
	    session_id, LOGIN_URL);
}

/* ****************************************** */

static void get_qsvar(const struct mg_request_info *request_info,
                      const char *name, char *dst, size_t dst_len) {
  const char *qs = request_info->query_string;
  mg_get_var(qs, strlen(qs == NULL ? "" : qs), name, dst, dst_len);
}

/* ****************************************** */

// A handler for the /authorize endpoint.
// Login page form sends user name and password to this endpoint.
static void authorize(struct mg_connection *conn,
                      const struct mg_request_info *request_info) {
  char user[32], password[32];

  if(!strcmp(request_info->request_method, "POST")) {
    char post_data[1024];    
    int post_data_len = mg_read(conn, post_data, sizeof(post_data));
    
    mg_get_var(post_data, post_data_len, "user", user, sizeof(user));
    mg_get_var(post_data, post_data_len, "password", password, sizeof(password));
  } else {
    // Fetch user name and password.
    get_qsvar(request_info, "user", user, sizeof(user));
    get_qsvar(request_info, "password", password, sizeof(password));
  }

  if (ntop->checkUserPassword(user, password)) {
    char key[256], session_id[64], random[64];

    // Authentication success:
    //   1. create new session
    //   2. set session ID token in the cookie
    //
    // The most secure way is to stay HTTPS all the time. However, just to
    // show the technique, we redirect to HTTP after the successful
    // authentication. The danger of doing this is that session cookie can
    // be stolen and an attacker may impersonate the user.
    // Secure application must use HTTPS all the time.

    snprintf(random, sizeof(random), "%d", rand());

    generate_session_id(session_id, random, user);

    // ntop->getTrace()->traceEvent(TRACE_ERROR, "==> %s\t%s", random, session_id);

    /* http://en.wikipedia.org/wiki/HTTP_cookie */
    mg_printf(conn, "HTTP/1.1 302 Found\r\n"
	      "Set-Cookie: session=%s; path=/; max-age=%u; HttpOnly\r\n"  // Session ID
	      "Set-Cookie: user=%s; path=/; max-age=%u; HttpOnly\r\n"  // Set user, needed by Javascript code
	      "Location: /\r\n\r\n",
	      session_id, HTTP_SESSION_DURATION, 
	      user, HTTP_SESSION_DURATION);
    
    /* Save session in redis */
    snprintf(key, sizeof(key), "sessions.%s", session_id);
    ntop->getRedis()->set(key, user, HTTP_SESSION_DURATION);
    ntop->getTrace()->traceEvent(TRACE_INFO, "[HTTP] Set session sessions.%s", session_id);

    snprintf(key, sizeof(key), "sessions.%s.ifname", session_id);
    ntop->getRedis()->del(key);
    ntop->getTrace()->traceEvent(TRACE_INFO, "[HTTP] Set sessions.%s.ifname", session_id);

    // ntop->getTrace()->traceEvent(TRACE_INFO, "[HTTP] Sending session %s", session_id);
  } else {
    // Authentication failure, redirect to login.
    redirect_to_login(conn, request_info);
  }
}

/* ****************************************** */

static void uri_encode(const char *src, char *dst, u_int dst_len) {
  u_int i = 0, j = 0;

  memset(dst, 0, dst_len);

  while(src[i] != '\0') {
    if(src[i] == '<') {
      dst[j++] = '&'; if(j == (dst_len-1)) break;
      dst[j++] = 'l'; if(j == (dst_len-1)) break;
      dst[j++] = 't'; if(j == (dst_len-1)) break;
      dst[j++] = ';'; if(j == (dst_len-1)) break;
    } else if(src[i] == '>') {
      dst[j++] = '&'; if(j == (dst_len-1)) break;
      dst[j++] = 'g'; if(j == (dst_len-1)) break;
      dst[j++] = 't'; if(j == (dst_len-1)) break;
      dst[j++] = ';'; if(j == (dst_len-1)) break;
    } else {
      dst[j++] = src[i]; if(j == (dst_len-1)) break;
    }

    i++;
  }
}

/* ****************************************** */

static int handle_lua_request(struct mg_connection *conn) {
  const struct mg_request_info *request_info = mg_get_request_info(conn);
  u_int len = strlen(request_info->uri);

  if((ntop->getGlobals()->isShutdown())
     //|| (strcmp(request_info->request_method, "GET"))
     || (ntop->getRedis() == NULL /* Starting up... */))
    return(send_error(conn, 403 /* Forbidden */, request_info->uri, "Unexpected HTTP method or ntopng still starting up..."));
  
#ifdef HAVE_SSL
  if(!request_info->is_ssl)
    redirect_to_ssl(conn, request_info);
#endif

  if(enable_users_login) {
    if((len > 4)
       && ((strcmp(&request_info->uri[len-4], ".css") == 0)
	   || (strcmp(&request_info->uri[len-3], ".js")) == 0))
      ;
    else if(!is_authorized(conn, request_info)) {
      redirect_to_login(conn, request_info);
    } else if(strcmp(request_info->uri, AUTHORIZE_URL) == 0) {
      authorize(conn, request_info);
      return(1);
    }
  }

  ntop->getTrace()->traceEvent(TRACE_INFO, "[HTTP] %s", request_info->uri);

  if(strstr(request_info->uri, "//")
     || strstr(request_info->uri, "&&")
     || strstr(request_info->uri, "??")
     || strstr(request_info->uri, "..")) {
    ntop->getTrace()->traceEvent(TRACE_WARNING, "[HTTP] The URL %s is invalid/dangerous", 
				 request_info->uri);
    return(send_error(conn, 400 /* Bad Request */, request_info->uri, 
		      "The URL specified contains invalid/dangerous characters"));
  }

  if((strncmp(request_info->uri, "/lua/", 5) == 0)
     || (strcmp(request_info->uri, "/") == 0)) {
    /* Lua Script */
    char path[255] = { 0 }, uri[2048];
    struct stat buf;

    snprintf(path, sizeof(path), "%s%s", httpserver->get_scripts_dir(),
	     (strlen(request_info->uri) == 1) ? "/lua/index.lua" : request_info->uri);
    
    ntop->fixPath(path);
    if((stat(path, &buf) == 0) && (S_ISREG (buf.st_mode))) {
      Lua *l = new Lua();
      
      ntop->getTrace()->traceEvent(TRACE_INFO, "[HTTP] %s [%s]", request_info->uri, path);
      
      if(l == NULL) {
	ntop->getTrace()->traceEvent(TRACE_ERROR, "[HTTP] Unable to start Lua interpreter");
	return(send_error(conn, 500 /* Internal server error */, 
			  "Internal server error", "%s", "Unable to start Lua interpreter"));
      } else {
	l->handle_script_request(conn, request_info, path);
	delete l;
	return(1); /* Handled */
      }
    }
    
    uri_encode(request_info->uri, uri, sizeof(uri)-1);
    
    return(send_error(conn, 404, "Not Found", PAGE_NOT_FOUND, uri));
  } else
    return(0); /* This is a static document so let mongoose handle it */
}

/* ****************************************** */

HTTPserver::HTTPserver(u_int16_t _port, const char *_docs_dir, const char *_scripts_dir) {
  struct mg_callbacks callbacks;
  static char ports[32];
  
  port = _port, docs_dir = strdup(_docs_dir), scripts_dir = strdup(_scripts_dir);
  httpserver = this;

#ifdef HAVE_SSL
  snprintf(ports, sizeof(ports), "%d,%ds", port, port+1);
#else
  snprintf(ports, sizeof(ports), "%d", port);
#endif

  static char *http_options[] = { 
    (char*)"listening_ports", ports, 
    (char*)"enable_directory_listing", (char*)"no",
    (char*)"document_root",  (char*)_docs_dir,
    (char*)"extra_mime_types", (char*)".inc=text/html,.css=text/css,.js=application/javascript",
    (char*)"num_threads", (char*)"5",
#ifdef HAVE_SSL
    (char*)"ssl_certificate", (char*)"ntop-cert.pem",
#endif
    NULL
  };

  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.begin_request = handle_lua_request;

  httpd_v4 = mg_start(&callbacks, NULL, (const char**)http_options);
  
  if(httpd_v4 == NULL) {
    ntop->getTrace()->traceEvent(TRACE_ERROR, "Unable to start HTTP server (IPv4) on port %d", port);
    exit(-1);
  }

#if 1/* TODO */
  httpd_v6 = NULL;
#endif

  /* ***************************** */

  ntop->getTrace()->traceEvent(TRACE_NORMAL, "HTTP server listening on port %d [%s][%s]",
				      port, docs_dir, scripts_dir);
};

/* ****************************************** */

HTTPserver::~HTTPserver() {
  if(httpd_v4) mg_stop(httpd_v4);
  if(httpd_v6) mg_stop(httpd_v6);

  free(docs_dir), free(scripts_dir);
  ntop->getTrace()->traceEvent(TRACE_NORMAL, "HTTP server terminated");
};

/* ****************************************** */
