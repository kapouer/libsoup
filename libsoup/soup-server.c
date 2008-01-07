/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * soup-server.c: Asynchronous HTTP server
 *
 * Copyright (C) 2001-2003, Ximian, Inc.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "soup-server.h"
#include "soup-address.h"
#include "soup-auth-domain.h"
#include "soup-date.h"
#include "soup-form.h"
#include "soup-headers.h"
#include "soup-message-private.h"
#include "soup-marshal.h"
#include "soup-path-map.h" 
#include "soup-socket.h"
#include "soup-ssl.h"

G_DEFINE_TYPE (SoupServer, soup_server, G_TYPE_OBJECT)

enum {
	REQUEST_STARTED,
	REQUEST_READ,
	REQUEST_FINISHED,
	REQUEST_ABORTED,
	LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

struct SoupClientContext {
	SoupServer     *server;
	SoupSocket     *sock;
	SoupAuthDomain *auth_domain;
	char           *auth_user;
};

typedef struct {
	char                   *path;

	SoupServerCallback      callback;
	GDestroyNotify          destroy;
	gpointer                user_data;
} SoupServerHandler;

typedef struct {
	SoupAddress       *interface;
	guint              port;

	char              *ssl_cert_file, *ssl_key_file;
	SoupSSLCredentials *ssl_creds;

	GMainLoop         *loop;

	SoupSocket        *listen_sock;
	GSList            *client_socks;

	gboolean           raw_paths;
	SoupPathMap       *handlers;
	SoupServerHandler *default_handler;
	
	GSList            *auth_domains;

	GMainContext      *async_context;
} SoupServerPrivate;
#define SOUP_SERVER_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), SOUP_TYPE_SERVER, SoupServerPrivate))

enum {
	PROP_0,

	PROP_PORT,
	PROP_INTERFACE,
	PROP_SSL_CERT_FILE,
	PROP_SSL_KEY_FILE,
	PROP_ASYNC_CONTEXT,
	PROP_RAW_PATHS,

	LAST_PROP
};

static GObject *constructor (GType                  type,
			     guint                  n_construct_properties,
			     GObjectConstructParam *construct_properties);
static void set_property (GObject *object, guint prop_id,
			  const GValue *value, GParamSpec *pspec);
static void get_property (GObject *object, guint prop_id,
			  GValue *value, GParamSpec *pspec);

static void
free_handler (SoupServerHandler *hand)
{
	g_free (hand->path);
	g_slice_free (SoupServerHandler, hand);
}

static void
soup_server_init (SoupServer *server)
{
	SoupServerPrivate *priv = SOUP_SERVER_GET_PRIVATE (server);

	priv->handlers = soup_path_map_new ((GDestroyNotify)free_handler);
}

static void
finalize (GObject *object)
{
	SoupServer *server = SOUP_SERVER (object);
	SoupServerPrivate *priv = SOUP_SERVER_GET_PRIVATE (server);
	GSList *iter;

	if (priv->interface)
		g_object_unref (priv->interface);

	g_free (priv->ssl_cert_file);
	g_free (priv->ssl_key_file);
	if (priv->ssl_creds)
		soup_ssl_free_server_credentials (priv->ssl_creds);

	if (priv->listen_sock)
		g_object_unref (priv->listen_sock);

	while (priv->client_socks) {
		SoupSocket *sock = priv->client_socks->data;

		soup_socket_disconnect (sock);
		priv->client_socks =
			g_slist_remove (priv->client_socks, sock);
	}

	if (priv->default_handler)
		free_handler (priv->default_handler);
	soup_path_map_free (priv->handlers);

	for (iter = priv->auth_domains; iter; iter = iter->next)
		g_object_unref (iter->data);
	g_slist_free (priv->auth_domains);

	if (priv->loop)
		g_main_loop_unref (priv->loop);
	if (priv->async_context)
		g_main_context_unref (priv->async_context);

	G_OBJECT_CLASS (soup_server_parent_class)->finalize (object);
}

static void
soup_server_class_init (SoupServerClass *server_class)
{
	GObjectClass *object_class = G_OBJECT_CLASS (server_class);

	g_type_class_add_private (server_class, sizeof (SoupServerPrivate));

	/* virtual method override */
	object_class->constructor = constructor;
	object_class->finalize = finalize;
	object_class->set_property = set_property;
	object_class->get_property = get_property;

	/* signals */

	/**
	 * SoupServer::request-started
	 * @server: the server
	 * @client: the client context
	 * @message: the new message
	 *
	 * Emitted when the server has started reading a new request.
	 * @message will be completely blank; not even the
	 * Request-Line will have been read yet. About the only thing
	 * you can usefully do with it is connect to its signals.
	 *
	 * If the request is read successfully, this will eventually
	 * be followed by a #SoupServer::request_read signal. If a
	 * response is then sent, the request processing will end with
	 * a #SoupServer::request_finished signal. If a network error
	 * occurs, the processing will instead end with
	 * #SoupServer::request_aborted.
	 **/
	signals[REQUEST_STARTED] =
		g_signal_new ("request-started",
			      G_OBJECT_CLASS_TYPE (object_class),
			      G_SIGNAL_RUN_FIRST,
			      G_STRUCT_OFFSET (SoupServerClass, request_started),
			      NULL, NULL,
			      soup_marshal_NONE__POINTER_OBJECT,
			      G_TYPE_NONE, 2,
			      SOUP_TYPE_CLIENT_CONTEXT,
			      SOUP_TYPE_MESSAGE);

	/**
	 * SoupServer::request-read
	 * @server: the server
	 * @client: the client context
	 * @message: the message
	 *
	 * Emitted when the server has successfully read a request.
	 * @message will have all of its request-side information
	 * filled in, and if the message was authenticated, @client
	 * will have information about that. This signal is emitted
	 * before any handlers are called for the message, and if it
	 * sets the message's #status_code, then normal handler
	 * processing will be skipped.
	 **/
	signals[REQUEST_READ] =
		g_signal_new ("request-read",
			      G_OBJECT_CLASS_TYPE (object_class),
			      G_SIGNAL_RUN_FIRST,
			      G_STRUCT_OFFSET (SoupServerClass, request_read),
			      NULL, NULL,
			      soup_marshal_NONE__POINTER_OBJECT,
			      G_TYPE_NONE, 2,
			      SOUP_TYPE_CLIENT_CONTEXT,
			      SOUP_TYPE_MESSAGE);

	/**
	 * SoupServer::request-finished
	 * @server: the server
	 * @client: the client context
	 * @message: the message
	 *
	 * Emitted when the server has finished writing a response to
	 * a request.
	 **/
	signals[REQUEST_FINISHED] =
		g_signal_new ("request-finished",
			      G_OBJECT_CLASS_TYPE (object_class),
			      G_SIGNAL_RUN_FIRST,
			      G_STRUCT_OFFSET (SoupServerClass, request_finished),
			      NULL, NULL,
			      soup_marshal_NONE__POINTER_OBJECT,
			      G_TYPE_NONE, 2,
			      SOUP_TYPE_CLIENT_CONTEXT,
			      SOUP_TYPE_MESSAGE);

	/**
	 * SoupServer::request-aborted
	 * @server: the server
	 * @client: the client context
	 * @message: the message
	 *
	 * Emitted when processing has failed for a message; this
	 * could mean either that it could not be read (if
	 * #SoupServer::request_read has not been emitted for it yet),
	 * or that the response could not be written back (if
	 * #SoupServer::request_read has been emitted but
	 * #SoupServer::request_finished has not been).
	 *
	 * @message is in an undefined state when this signal is
	 * emitted; the signal exists primarily to allow the server to
	 * free any state that it may have allocated in
	 * #SoupServer::request_started.
	 **/
	signals[REQUEST_ABORTED] =
		g_signal_new ("request-aborted",
			      G_OBJECT_CLASS_TYPE (object_class),
			      G_SIGNAL_RUN_FIRST,
			      G_STRUCT_OFFSET (SoupServerClass, request_aborted),
			      NULL, NULL,
			      soup_marshal_NONE__POINTER_OBJECT,
			      G_TYPE_NONE, 2,
			      SOUP_TYPE_CLIENT_CONTEXT,
			      SOUP_TYPE_MESSAGE);

	/* properties */
	g_object_class_install_property (
		object_class, PROP_PORT,
		g_param_spec_uint (SOUP_SERVER_PORT,
				   "Port",
				   "Port to listen on",
				   0, 65536, 0,
				   G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
	g_object_class_install_property (
		object_class, PROP_INTERFACE,
		g_param_spec_object (SOUP_SERVER_INTERFACE,
				     "Interface",
				     "Address of interface to listen on",
				     SOUP_TYPE_ADDRESS,
				     G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
	g_object_class_install_property (
		object_class, PROP_SSL_CERT_FILE,
		g_param_spec_string (SOUP_SERVER_SSL_CERT_FILE,
				     "SSL certificate file",
				     "File containing server SSL certificate",
				     NULL,
				     G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
	g_object_class_install_property (
		object_class, PROP_SSL_KEY_FILE,
		g_param_spec_string (SOUP_SERVER_SSL_KEY_FILE,
				     "SSL key file",
				     "File containing server SSL key",
				     NULL,
				     G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
	g_object_class_install_property (
		object_class, PROP_ASYNC_CONTEXT,
		g_param_spec_pointer (SOUP_SERVER_ASYNC_CONTEXT,
				      "Async GMainContext",
				      "The GMainContext to dispatch async I/O in",
				      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
	g_object_class_install_property (
		object_class, PROP_RAW_PATHS,
		g_param_spec_boolean (SOUP_SERVER_RAW_PATHS,
				      "Raw paths",
				      "If %TRUE, percent-encoding in the Request-URI path will not be automatically decoded.",
				      FALSE,
				      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
}

static GObject *
constructor (GType                  type,
	     guint                  n_construct_properties,
	     GObjectConstructParam *construct_properties)
{
	GObject *server;
	SoupServerPrivate *priv;

	server = G_OBJECT_CLASS (soup_server_parent_class)->constructor (
		type, n_construct_properties, construct_properties);
	if (!server)
		return NULL;
	priv = SOUP_SERVER_GET_PRIVATE (server);

	if (!priv->interface) {
		priv->interface =
			soup_address_new_any (SOUP_ADDRESS_FAMILY_IPV4,
					      priv->port);
	}

	if (priv->ssl_cert_file && priv->ssl_key_file) {
		priv->ssl_creds = soup_ssl_get_server_credentials (
			priv->ssl_cert_file,
			priv->ssl_key_file);
		if (!priv->ssl_creds) {
			g_object_unref (server);
			return NULL;
		}
	}

	priv->listen_sock =
		soup_socket_new (SOUP_SOCKET_LOCAL_ADDRESS, priv->interface,
				 SOUP_SOCKET_SSL_CREDENTIALS, priv->ssl_creds,
				 SOUP_SOCKET_ASYNC_CONTEXT, priv->async_context,
				 NULL);
	if (!soup_socket_listen (priv->listen_sock)) {
		g_object_unref (server);
		return NULL;
	}

	/* Re-resolve the interface address, in particular in case
	 * the passed-in address had SOUP_ADDRESS_ANY_PORT.
	 */
	g_object_unref (priv->interface);
	priv->interface = soup_socket_get_local_address (priv->listen_sock);
	g_object_ref (priv->interface);
	priv->port = soup_address_get_port (priv->interface);

	return server;
}

static void
set_property (GObject *object, guint prop_id,
	      const GValue *value, GParamSpec *pspec)
{
	SoupServerPrivate *priv = SOUP_SERVER_GET_PRIVATE (object);

	switch (prop_id) {
	case PROP_PORT:
		priv->port = g_value_get_uint (value);
		break;
	case PROP_INTERFACE:
		if (priv->interface)
			g_object_unref (priv->interface);
		priv->interface = g_value_get_object (value);
		if (priv->interface)
			g_object_ref (priv->interface);
		break;
	case PROP_SSL_CERT_FILE:
		priv->ssl_cert_file =
			g_strdup (g_value_get_string (value));
		break;
	case PROP_SSL_KEY_FILE:
		priv->ssl_key_file =
			g_strdup (g_value_get_string (value));
		break;
	case PROP_ASYNC_CONTEXT:
		priv->async_context = g_value_get_pointer (value);
		if (priv->async_context)
			g_main_context_ref (priv->async_context);
		break;
	case PROP_RAW_PATHS:
		priv->raw_paths = g_value_get_boolean (value);
		break;
	default:
		break;
	}
}

static void
get_property (GObject *object, guint prop_id,
	      GValue *value, GParamSpec *pspec)
{
	SoupServerPrivate *priv = SOUP_SERVER_GET_PRIVATE (object);

	switch (prop_id) {
	case PROP_PORT:
		g_value_set_uint (value, priv->port);
		break;
	case PROP_INTERFACE:
		g_value_set_object (value, priv->interface);
		break;
	case PROP_SSL_CERT_FILE:
		g_value_set_string (value, priv->ssl_cert_file);
		break;
	case PROP_SSL_KEY_FILE:
		g_value_set_string (value, priv->ssl_key_file);
		break;
	case PROP_ASYNC_CONTEXT:
		g_value_set_pointer (value, priv->async_context ? g_main_context_ref (priv->async_context) : NULL);
		break;
	case PROP_RAW_PATHS:
		g_value_set_boolean (value, priv->raw_paths);
		break;
	default:
		break;
	}
}

/**
 * soup_server_new:
 * @optname1: name of first property to set
 * @...: value of @optname1, followed by additional property/value pairs
 *
 * Creates a new #SoupServer.
 **/
SoupServer *
soup_server_new (const char *optname1, ...)
{
	SoupServer *server;
	va_list ap;

	va_start (ap, optname1);
	server = (SoupServer *)g_object_new_valist (SOUP_TYPE_SERVER,
						    optname1, ap);
	va_end (ap);

	return server;
}

/**
 * soup_server_get_port:
 * @server: a #SoupServer
 *
 * Gets the TCP port that @server is listening on. This is most useful
 * when you did not request a specific port (or explicitly requested
 * %SOUP_ADDRESS_ANY_PORT).
 *
 * Return value: the port @server is listening on.
 **/
guint
soup_server_get_port (SoupServer *server)
{
	g_return_val_if_fail (SOUP_IS_SERVER (server), 0);

	return SOUP_SERVER_GET_PRIVATE (server)->port;
}

/**
 * soup_server_is_https:
 * @server: a #SoupServer
 *
 * Checks whether @server is running plain http or https.
 *
 * In order for a server to run https, you must set the
 * %SOUP_SERVER_SSL_CERT_FILE and %SOUP_SERVER_SSL_KEY_FILE properties
 * to provide it with an SSL certificate to use.
 *
 * Return value: %TRUE if @server is serving https.
 **/
gboolean
soup_server_is_https (SoupServer *server)
{
	SoupServerPrivate *priv;

	g_return_val_if_fail (SOUP_IS_SERVER (server), 0);
	priv = SOUP_SERVER_GET_PRIVATE (server);

	return (priv->ssl_cert_file && priv->ssl_key_file);
}

/**
 * soup_server_get_listener:
 * @server: a #SoupServer
 *
 * Gets @server's listening socket. You should treat this as
 * read-only; writing to it or modifiying it may cause @server to
 * malfunction.
 *
 * Return value: the listening socket.
 **/
SoupSocket *
soup_server_get_listener (SoupServer *server)
{
	SoupServerPrivate *priv;

	g_return_val_if_fail (SOUP_IS_SERVER (server), NULL);
	priv = SOUP_SERVER_GET_PRIVATE (server);

	return priv->listen_sock;
}

static void start_request (SoupServer *, SoupClientContext *);

static void
soup_client_context_cleanup (SoupClientContext *client)
{
	if (client->auth_domain) {
		g_object_unref (client->auth_domain);
		client->auth_domain = NULL;
	}
	if (client->auth_user) {
		g_free (client->auth_user);
		client->auth_user = NULL;
	}
}

static void
request_finished (SoupMessage *msg, SoupClientContext *client)
{
	SoupServer *server = client->server;
	SoupSocket *sock = client->sock;

	g_signal_emit (server,
		       msg->status_code == SOUP_STATUS_IO_ERROR ?
		       signals[REQUEST_ABORTED] : signals[REQUEST_FINISHED],
		       0, client, msg);

	soup_client_context_cleanup (client);
	if (soup_socket_is_connected (sock) && soup_message_is_keepalive (msg)) {
		/* Start a new request */
		start_request (server, client);
	} else {
		soup_socket_disconnect (sock);
		g_slice_free (SoupClientContext, client);
	}
	g_object_unref (msg);
	g_object_unref (sock);
}

static SoupServerHandler *
soup_server_get_handler (SoupServer *server, const char *path)
{
	SoupServerPrivate *priv;
	SoupServerHandler *hand;

	g_return_val_if_fail (SOUP_IS_SERVER (server), NULL);
	priv = SOUP_SERVER_GET_PRIVATE (server);

	if (path) {
		hand = soup_path_map_lookup (priv->handlers, path);
		if (hand)
			return hand;
	}
	return priv->default_handler;
}

static void
got_headers (SoupMessage *req, SoupClientContext *client)
{
	SoupServer *server = client->server;
	SoupServerPrivate *priv = SOUP_SERVER_GET_PRIVATE (server);
	SoupURI *uri;
	SoupDate *date;
	char *date_string;
	SoupAuthDomain *domain;
	GSList *iter;
	gboolean rejected = FALSE;
	char *auth_user;

	if (!priv->raw_paths) {
		uri = soup_message_get_uri (req);
		soup_uri_decode (uri->path);
	}

	/* Add required response headers */
	date = soup_date_new_from_now (0);
	date_string = soup_date_to_string (date, SOUP_DATE_HTTP);
	soup_message_headers_replace (req->response_headers, "Date",
				      date_string);
	g_free (date_string);
	soup_date_free (date);
	
	/* Now handle authentication. (We do this here so that if
	 * the request uses "Expect: 100-continue", we can reject it
	 * immediately rather than waiting for the request body to
	 * be sent.
	 */
	for (iter = priv->auth_domains; iter; iter = iter->next) {
		domain = iter->data;

		if (soup_auth_domain_covers (domain, req)) {
			auth_user = soup_auth_domain_accepts (domain, req);
			if (auth_user) {
				client->auth_domain = g_object_ref (domain);
				client->auth_user = auth_user;
				return;
			}

			rejected = TRUE;
		}
	}

	/* If no auth domain rejected it, then it's ok. */
	if (!rejected)
		return;

	for (iter = priv->auth_domains; iter; iter = iter->next) {
		domain = iter->data;

		if (soup_auth_domain_covers (domain, req))
			soup_auth_domain_challenge (domain, req);
	}
}

static void
call_handler (SoupMessage *req, SoupClientContext *client)
{
	SoupServer *server = client->server;
	SoupServerHandler *hand;
	SoupURI *uri;

	if (req->status_code != 0)
		return;

	uri = soup_message_get_uri (req);
	hand = soup_server_get_handler (server, uri->path);
	if (!hand) {
		soup_message_set_status (req, SOUP_STATUS_NOT_FOUND);
		return;
	}

	if (hand->callback) {
		GHashTable *form_data_set;

		if (uri->query)
			form_data_set = soup_form_decode_urlencoded (uri->query);
		else
			form_data_set = NULL;

		/* Call method handler */
		(*hand->callback) (server, req,
				   uri->path, form_data_set,
				   client, hand->user_data);

		if (form_data_set)
			g_hash_table_destroy (form_data_set);
	}
}

static void
start_request (SoupServer *server, SoupClientContext *client)
{
	SoupMessage *msg;

	soup_client_context_cleanup (client);

	/* Listen for another request on this connection */
	msg = g_object_new (SOUP_TYPE_MESSAGE, NULL);
        soup_message_headers_set_encoding (msg->response_headers,
                                           SOUP_ENCODING_CONTENT_LENGTH);

	g_signal_connect (msg, "got_headers", G_CALLBACK (got_headers), client);
	g_signal_connect (msg, "got_body", G_CALLBACK (call_handler), client);
	g_signal_connect (msg, "finished", G_CALLBACK (request_finished), client);

	g_signal_emit (server, signals[REQUEST_STARTED], 0,
		       client, msg);

	g_object_ref (client->sock);
	soup_message_read_request (msg, client->sock);
}

static void
socket_disconnected (SoupSocket *sock, SoupServer *server)
{
	SoupServerPrivate *priv = SOUP_SERVER_GET_PRIVATE (server);

	priv->client_socks = g_slist_remove (priv->client_socks, sock);
	g_signal_handlers_disconnect_by_func (sock, socket_disconnected, server);
	g_object_unref (sock);
}

static void
new_connection (SoupSocket *listner, SoupSocket *sock, gpointer user_data)
{
	SoupServer *server = user_data;
	SoupServerPrivate *priv = SOUP_SERVER_GET_PRIVATE (server);
	SoupClientContext *client = g_slice_new0 (SoupClientContext);

	client->server = server;
	client->sock = g_object_ref (sock);
	priv->client_socks = g_slist_prepend (priv->client_socks, sock);
	g_signal_connect (sock, "disconnected",
			  G_CALLBACK (socket_disconnected), server);
	start_request (server, client);
}

/**
 * soup_server_run_async:
 * @server: a #SoupServer
 *
 * Starts @server, causing it to listen for and process incoming
 * connections.
 *
 * The server actually runs in @server's #GMainContext. It will not
 * actually perform any processing unless the appropriate main loop is
 * running. In the simple case where you did not set the server's
 * %SOUP_SERVER_ASYNC_CONTEXT property, this means the server will run
 * whenever the glib main loop is running.
 *
 * soup_server_run_async() refs @server, so you should run
 * soup_server_quit() to unref it when you are done.
 **/
void
soup_server_run_async (SoupServer *server)
{
	SoupServerPrivate *priv;

	g_return_if_fail (SOUP_IS_SERVER (server));
	priv = SOUP_SERVER_GET_PRIVATE (server);

	if (!priv->listen_sock) {
		if (priv->loop) {
			g_main_loop_unref (priv->loop);
			priv->loop = NULL;
		}
		return;
	}

	g_signal_connect (priv->listen_sock, "new_connection",
			  G_CALLBACK (new_connection), server);
	g_object_ref (server);

	return;

}

/**
 * soup_server_run:
 * @server: a #SoupServer
 *
 * Starts @server, causing it to listen for and process incoming
 * connections. Unlike soup_server_run_async(), this creates a
 * #GMainLoop and runs it, and it will not return until someone calls
 * soup_server_quit() to stop the server.
 **/
void
soup_server_run (SoupServer *server)
{
	SoupServerPrivate *priv;

	g_return_if_fail (SOUP_IS_SERVER (server));
	priv = SOUP_SERVER_GET_PRIVATE (server);

	if (!priv->loop) {
		priv->loop = g_main_loop_new (priv->async_context, TRUE);
		soup_server_run_async (server);
	}

	if (priv->loop)
		g_main_loop_run (priv->loop);
}

/**
 * soup_server_quit:
 * @server: a #SoupServer
 *
 * Stops processing for @server. Call this to clean up after
 * soup_server_run_async(), or to terminate a call to soup_server_run().
 *
 * @server is still in a working state after this call; you can start
 * and stop a server as many times as you want.
 **/
void
soup_server_quit (SoupServer *server)
{
	SoupServerPrivate *priv;

	g_return_if_fail (SOUP_IS_SERVER (server));
	priv = SOUP_SERVER_GET_PRIVATE (server);

	g_signal_handlers_disconnect_by_func (priv->listen_sock,
					      G_CALLBACK (new_connection),
					      server);
	if (priv->loop)
		g_main_loop_quit (priv->loop);

	g_object_unref (server);
}

/**
 * soup_server_get_async_context:
 * @server: a #SoupServer
 *
 * Gets @server's async_context. This does not add a ref to the
 * context, so you will need to ref it yourself if you want it to
 * outlive its server.
 *
 * Return value: @server's #GMainContext, which may be %NULL
 **/
GMainContext *
soup_server_get_async_context (SoupServer *server)
{
	SoupServerPrivate *priv;

	g_return_val_if_fail (SOUP_IS_SERVER (server), NULL);
	priv = SOUP_SERVER_GET_PRIVATE (server);

	return priv->async_context;
}

/**
 * SoupClientContext:
 *
 * A #SoupClientContext provides additional information about the
 * client making a particular request. In particular, you can use
 * soup_client_context_get_auth_domain() and
 * soup_client_context_get_auth_user() to determine if HTTP
 * authentication was used successfully.
 *
 * soup_client_context_get_address() and/or
 * soup_client_context_get_host() can be used to get information for
 * logging or debugging purposes. soup_client_context_get_socket() may
 * also be of use in some situations (eg, tracking when multiple
 * requests are made on the same connection).
 **/
GType
soup_client_context_get_type (void)
{
	static GType type = 0;

	if (type == 0)
		type = g_pointer_type_register_static ("SoupClientContext");
	return type;
}

/**
 * soup_client_context_get_socket:
 * @client: a #SoupClientContext
 *
 * Retrieves the #SoupSocket that @client is associated with.
 *
 * If you are using this method to observe when multiple requests are
 * made on the same persistent HTTP connection (eg, as the ntlm-test
 * test program does), you will need to pay attention to socket
 * destruction as well (either by using weak references, or by
 * connecting to the #SoupSocket::disconnected signal), so that you do
 * not get fooled when the allocator reuses the memory address of a
 * previously-destroyed socket to represent a new socket.
 *
 * Return value: the #SoupSocket that @client is associated with.
 **/
SoupSocket *
soup_client_context_get_socket (SoupClientContext *client)
{
	g_return_val_if_fail (client != NULL, NULL);

	return client->sock;
}

/**
 * soup_client_context_get_address:
 * @client: a #SoupClientContext
 *
 * Retrieves the #SoupAddress associated with the remote end
 * of a connection.
 *
 * Return value: the #SoupAddress associated with the remote end of a
 * connection.
 **/
SoupAddress *
soup_client_context_get_address (SoupClientContext *client)
{
	g_return_val_if_fail (client != NULL, NULL);

	return soup_socket_get_remote_address (client->sock);
}

/**
 * soup_client_context_get_host:
 * @client: a #SoupClientContext
 *
 * Retrieves the IP address associated with the remote end of a
 * connection. (If you want the actual hostname, you'll have to call
 * soup_client_context_get_address() and then call the appropriate
 * #SoupAddress method to resolve it.)
 *
 * Return value: the IP address associated with the remote end of a
 * connection.
 **/
const char *
soup_client_context_get_host (SoupClientContext *client)
{
	SoupAddress *address;

	address = soup_client_context_get_address (client);
	return soup_address_get_physical (address);
}

/**
 * soup_client_context_get_auth_domain:
 * @client: a #SoupClientContext
 *
 * Checks whether the request associated with @client has been
 * authenticated, and if so returns the #SoupAuthDomain that
 * authenticated it.
 *
 * Return value: a #SoupAuthDomain, or %NULL if the request was not
 * authenticated.
 **/
SoupAuthDomain *
soup_client_context_get_auth_domain (SoupClientContext *client)
{
	g_return_val_if_fail (client != NULL, NULL);

	return client->auth_domain;
}

/**
 * soup_client_context_get_auth_user:
 * @client: a #SoupClientContext
 *
 * Checks whether the request associated with @client has been
 * authenticated, and if so returns the username that the client
 * authenticated as.
 *
 * Return value: the authenticated-as user, or %NULL if the request
 * was not authenticated.
 **/
const char *
soup_client_context_get_auth_user (SoupClientContext *client)
{
	g_return_val_if_fail (client != NULL, NULL);

	return client->auth_user;
}

/**
 * SoupServerCallback:
 * @server: the #SoupServer
 * @msg: the message being processed
 * @path: the path component of @msg's Request-URI
 * @query: the parsed query component of @msg's Request-URI
 * @client: additional contextual information about the client
 * @user_data: the data passed to @soup_server_add_handler
 *
 * A callback used to handle requests to a #SoupServer. The callback
 * will be invoked after receiving the request body; @msg's %method,
 * %request_headers, and %request_body fields will be filled in.
 *
 * @path and @query contain the likewise-named components of the
 * Request-URI, subject to certain assumptions. By default,
 * #SoupServer decodes all percent-encoding in the URI path, such that
 * "/foo%%2Fbar" is treated the same as "/foo/bar". If your server is
 * serving resources in some non-POSIX-filesystem namespace, you may
 * want to distinguish those as two distinct paths. In that case, you
 * can set the %SOUP_SERVER_RAW_PATHS property when creating the
 * #SoupServer, and it will leave those characters undecoded. (You may
 * want to call soup_uri_normalize() to decode any percent-encoded
 * characters that you aren't handling specially.)
 *
 * @query contains the query component of the Request-URI parsed
 * according to the rules for HTML form handling. Although this is the
 * only commonly-used query string format in HTTP, there is nothing
 * that actually requires that HTTP URIs use that format; if your
 * server needs to use some other format, you can just ignore @query,
 * and call soup_message_get_uri() and parse the URI's query field
 * yourself.
 *
 * After determining what to do with the request, the callback must at
 * a minimum call soup_message_set_status() (or
 * soup_message_set_status_full()) on @msg to set the response status
 * code. Additionally, it may set response headers and/or fill in the
 * response body.
 *
 * If the callback cannot fully fill in the response before returning
 * (eg, if it needs to wait for information from a database, or
 * another network server), it should call soup_server_pause_message()
 * to tell #SoupServer to not send the response right away. When the
 * response is ready, call soup_server_unpause_message() to cause it
 * to be sent.
 *
 * To send the response body a bit at a time using "chunked" encoding,
 * first call soup_message_headers_set_encoding() to set
 * %SOUP_ENCODING_CHUNKED on the %response_headers. Then call
 * soup_message_body_append() (or soup_message_body_append_buffer())
 * to append each chunk as it becomes ready, and
 * soup_server_unpause_message() to make sure it's running. (The
 * server will automatically pause the message if it is using chunked
 * encoding but no more chunks are available.) When you are done, call
 * soup_message_body_complete() to indicate that no more chunks are
 * coming.
 **/

/**
 * soup_server_add_handler:
 * @server: a #SoupServer
 * @path: the toplevel path for the handler
 * @callback: callback to invoke for requests under @path
 * @user_data: data for @callback
 * @destroy: destroy notifier to free @user_data
 *
 * Adds a handler to @server for requests under @path. See the
 * documentation for #SoupServerCallback for information about
 * how callbacks should behave.
 **/
void
soup_server_add_handler (SoupServer            *server,
			 const char            *path,
			 SoupServerCallback     callback,
			 gpointer               user_data,
			 GDestroyNotify         destroy)
{
	SoupServerPrivate *priv;
	SoupServerHandler *hand;

	g_return_if_fail (SOUP_IS_SERVER (server));
	g_return_if_fail (callback != NULL);
	priv = SOUP_SERVER_GET_PRIVATE (server);

	hand = g_slice_new0 (SoupServerHandler);
	hand->path       = g_strdup (path);
	hand->callback   = callback;
	hand->destroy    = destroy;
	hand->user_data  = user_data;

	soup_server_remove_handler (server, path);
	if (path)
		soup_path_map_add (priv->handlers, path, hand);
	else
		priv->default_handler = hand;
}

static void
unregister_handler (SoupServerHandler *handler)
{
	if (handler->destroy)
		handler->destroy (handler->user_data);
}

/**
 * soup_server_remove_handler:
 * @server: a #SoupServer
 * @path: the toplevel path for the handler
 *
 * Removes the handler registered at @path.
 **/
void
soup_server_remove_handler (SoupServer *server, const char *path)
{
	SoupServerPrivate *priv;
	SoupServerHandler *hand;

	g_return_if_fail (SOUP_IS_SERVER (server));
	priv = SOUP_SERVER_GET_PRIVATE (server);

	if (!path) {
		if (priv->default_handler) {
			unregister_handler (priv->default_handler);
			free_handler (priv->default_handler);
			priv->default_handler = NULL;
		}
		return;
	}

	hand = soup_path_map_lookup (priv->handlers, path);
	if (hand && !strcmp (path, hand->path)) {
		unregister_handler (hand);
		soup_path_map_remove (priv->handlers, path);
	}
}

/**
 * soup_server_add_auth_domain:
 * @server: a #SoupServer
 * @auth_domain: a #SoupAuthDomain
 *
 * Adds an authentication domain to @server. Each auth domain will
 * have the chance to require authentication for each request that
 * comes in; normally auth domains will require authentication for
 * requests on certain paths that they have been set up to watch, or
 * that meet other criteria set by the caller. If an auth domain
 * determines that a request requires authentication (and the request
 * doesn't contain authentication), @server will automatically reject
 * the request with an appropriate status (401 Unauthorized or 407
 * Proxy Authentication Required). If the request used the
 * "100-continue" Expectation, @server will reject it before the
 * request body is sent.
 **/
void
soup_server_add_auth_domain (SoupServer *server, SoupAuthDomain *auth_domain)
{
	SoupServerPrivate *priv;

	g_return_if_fail (SOUP_IS_SERVER (server));
	priv = SOUP_SERVER_GET_PRIVATE (server);

	priv->auth_domains = g_slist_prepend (priv->auth_domains, auth_domain);
}

/**
 * soup_server_remove_auth_domain:
 * @server: a #SoupServer
 * @auth_domain: a #SoupAuthDomain
 *
 * Removes @auth_domain from @server.
 **/
void
soup_server_remove_auth_domain (SoupServer *server, SoupAuthDomain *auth_domain)
{
	SoupServerPrivate *priv;

	g_return_if_fail (SOUP_IS_SERVER (server));
	priv = SOUP_SERVER_GET_PRIVATE (server);

	priv->auth_domains = g_slist_remove (priv->auth_domains, auth_domain);
	g_object_unref (auth_domain);
}

/**
 * soup_server_pause_message:
 * @server: a #SoupServer
 * @msg: a #SoupMessage associated with @server.
 *
 * Pauses I/O on @msg. This can be used when you need to return from
 * the server handler without having the full response ready yet. Use
 * soup_server_unpause_message() to resume I/O.
 **/
void
soup_server_pause_message (SoupServer *server,
			   SoupMessage *msg)
{
	g_return_if_fail (SOUP_IS_SERVER (server));
	g_return_if_fail (SOUP_IS_MESSAGE (msg));

	soup_message_io_unpause (msg);
}

/**
 * soup_server_unpause_message:
 * @server: a #SoupServer
 * @msg: a #SoupMessage associated with @server.
 *
 * Resumes I/O on @msg. Use this to resume after calling
 * soup_server_pause_message(), or after adding a new chunk to a
 * chunked response.
 *
 * I/O won't actually resume until you return to the main loop.
 **/
void
soup_server_unpause_message (SoupServer *server,
			     SoupMessage *msg)
{
	g_return_if_fail (SOUP_IS_SERVER (server));
	g_return_if_fail (SOUP_IS_MESSAGE (msg));

	soup_message_io_unpause (msg);
}

