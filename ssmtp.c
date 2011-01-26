/*
 * sSMTP -- send messages via SMTP to a mailhub for local delivery or forwarding.
 * This program is used in place of /usr/sbin/sendmail, called by "mail" (et al).
 * sSMTP does a selected subset of sendmail's standard tasks (including exactly
 * one rewriting task), and explains if you ask it to do something it can't. It
 * then sends the mail to the mailhub via an SMTP connection. Believe it or not,
 * this is nothing but a filter
 * 
 * See COPYRIGHT for the license
 * 
 */
#define VERSION "2.65"
#define _GNU_SOURCE

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/param.h>
#include <unistd.h>
#include <sys/types.h>
#include <stdlib.h>
#include <stdarg.h>
#include <syslog.h>
#include <setjmp.h>
#include <string.h>
#include <ctype.h>
#include <netdb.h>
#include <libgen.h>
#include <errno.h>
#include <fcntl.h>
#ifdef HAVE_SSL
#ifdef HAVE_GNUTLS
#include <gnutls/openssl.h>
#else
#include <openssl/crypto.h>
#include <openssl/x509.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#endif
#endif
#ifdef MD5AUTH
#include "md5auth/hmac_md5.h"
#endif
#include "ssmtp.h"
#include "xgethostname.h"
#include "arpadate.h"
#include "ccan/list/list.h"

/* Warning removal by defining defaults */
#ifndef REVALIASES_FILE
#define REVALIASES_FILE "/etc/ssmtp/revaliases"
#endif
#ifndef CONFIGURATION_FILE
#define CONFIGURATION_FILE "/etc/ssmtp/ssmtp.conf"
#endif
#ifdef HAVE_SASL
#include <sasl/sasl.h>
#include <sasl/saslutil.h>
#else
#include "base64.h"
#endif

#include <regex.h>

#define ARRAY_SIZE(x) sizeof(x) / sizeof(x[0])

bool_t have_date = False;
bool_t have_from = False;
#ifdef HASTO_OPTION
bool_t have_to = False;
#endif
bool_t minus_t = False;
bool_t minus_v = False;
bool_t override_from = False;
bool_t rewrite_domain = False;
bool_t use_tls = False;			/* Use SSL to transfer mail to HUB */
bool_t use_starttls = False;		/* SSL only after STARTTLS (RFC2487) */
bool_t use_cert = False;		/* Use a certificate to transfer SSL mail */
bool_t use_oldauth = False;		/* use old AUTH LOGIN username style */

#define ARPADATE_LENGTH 32		/* Current date in RFC format */
char arpadate[ARPADATE_LENGTH];
char *auth_user = '\0';
char *auth_pass = '\0';
char *auth_method = '\0';		/* Mechanism for SMTP authentication */
char *mail_domain = '\0';
char *from = '\0';		/* Use this as the From: address */
char *hostname;
char *minus_f = '\0';
char *minus_F = '\0';
char *gecos;
char *prog = '\0';
char *root = NULL;
char *tls_cert = "/etc/ssl/certs/ssmtp.pem";	/* Default Certificate */
char *uad = '\0';
char *config_file = '\0';		/* alternate configuration file */

FILE *input_tmpf = NULL;		/* temporary input filehandle - used only by dead_letter() */

struct hostport {
	char	*name;
	int	port;
} mailhost = { "mailhub", 25 };

#ifdef DEBUG
int log_level = 1;
#else
int log_level = 0;
#endif
int minuserid = MAXSYSUID+1;
#ifdef INET6
int p_family = PF_UNSPEC;		/* Protocol family used in SMTP connection */
#endif

jmp_buf TimeoutJmpBuf;			/* Timeout waiting for input from network */

static struct list_head rcpt_list = LIST_HEAD_INIT(rcpt_list);
static struct list_head header_list = LIST_HEAD_INIT(header_list);

#ifdef HAVE_SSL
SSL *ssl;
#endif

#ifdef MD5AUTH
static char hextab[]="0123456789abcdef";
#endif

ssize_t outbytes;

#if defined (__SVR4) && defined (__sun)
/*
strndup() - Unimplemented by the Solaris libc.
*/
char *strndup(char const *s, size_t n)
{
	size_t len = strlen(s, n);
	char *new = malloc(len + 1);
	if(new == NULL) return NULL;
	new[len] = '\0';
	return memcpy(new, s, len);
}
#endif

/*
 * log_event() -- Write event to syslog (or log file if defined)
 */
static void
log_event(int priority, char *format, ...)
{
	char buf[(BUF_SZ + 1)];
	va_list ap;

	va_start(ap, format);
	(void)vsnprintf(buf, BUF_SZ, format, ap);
	va_end(ap);

#ifdef LOGFILE
	FILE *fp;

	if((fp = fopen("/tmp/ssmtp.log", "a")) != (FILE *)NULL) {
		(void)fprintf(fp, "%s\n", buf);
		(void)fclose(fp);
	}
	else {
		(void)fprintf(stderr, "Can't write to /tmp/ssmtp.log\n");
	}
#endif

#if HAVE_SYSLOG_H
#if OLDSYSLOG
	openlog("sSMTP", LOG_PID);
#else
	openlog("sSMTP", LOG_PID, LOG_MAIL);
#endif
	syslog(priority, "%s", buf);
	closelog();
#endif
}

/*
 * dead_letter() -- Save stdin to ~/dead.letter if possible
 */
static void
dead_letter(void)
{
	char *path;
	char buf[(BUF_SZ + 1)];
	struct passwd *pw;
	uid_t uid;
	FILE *fp, *input;

	uid = getuid();
	pw = getpwuid(uid);

	if(isatty(fileno(stdin))) {
		if(log_level > 0) {
			log_event(LOG_ERR,
				"stdin is a TTY - not saving to %s/dead.letter",
				pw->pw_dir);
		}
		return;
	}

	if(pw == (struct passwd *)NULL) {
		/* Far to early to save things */
		if(log_level > 0) {
			log_event(LOG_ERR, "No sender failing horribly!");
		}
		return;
	}

#define DEAD_LETTER "/dead.letter"
	path = malloc (strlen (pw->pw_dir) + sizeof (DEAD_LETTER));
	if (!path) {
		/*
		 * Can't use die() here since dead_letter() is called from die()
		 */
		exit(1);
	}
	memcpy (path, pw->pw_dir, strlen (pw->pw_dir));
	memcpy (path + strlen (pw->pw_dir), DEAD_LETTER, sizeof (DEAD_LETTER));

	if((fp = fopen(path, "a")) == (FILE *)NULL) {
		/* Perhaps the person doesn't have a homedir... */
		if(log_level > 0) {
			log_event(
				LOG_ERR,
				"Can't open %s: %s; failing horribly!",
				path,
				strerror(errno)
			);
		}
		free(path);
		return;
	}

	/* We start on a new line with a blank line separating messages */
	(void)fprintf(fp, "\n\n");

	input = input_tmpf;
	if (input == NULL) {
		input = stdin;
	} else {
		/* rewind handle to beginning... */
		rewind(input);
	}

	while(fgets(buf, sizeof(buf), input)) {
		(void)fputs(buf, fp);
	}

	if(fclose(fp) == -1) {
		if(log_level > 0) {
			log_event(LOG_ERR,
				"Can't close %s/dead.letter: %s"
				, pw->pw_dir, strerror(errno));
		}
	}
	free(path);
}

/*
 * die() -- Write error message, dead.letter and exit
 */
static void
die(char *format, ...)
{
	char buf[(BUF_SZ + 1)];
	va_list ap;

	va_start(ap, format);
	(void)vsnprintf(buf, BUF_SZ, format, ap);
	va_end(ap);

	(void)fprintf(stderr, "%s: %s\n", prog, buf);
	log_event(LOG_ERR, "%s", buf);

	/* Send message to dead.letter */
	(void)dead_letter();

	exit(1);
}

/*
 * xbasename() -- Return last element of path
 */
char *xbasename(char *str)
{
	char *p;

	p = strrchr(str, '/');
	if (!p) {
		p = str;
	}

	return(strdup(p));
}

/*
 * strip_pre_ws() -- Return pointer to first non-whitespace character
 */
static char *
strip_pre_ws(char *str)
{
	char *p;

	p = str;
	while(*p && isspace(*p)) p++;

	return(p);
}

/*
 * strip_post_ws() -- Return pointer to last non-whitespace character
 */
static char *
strip_post_ws(char *str)
{
	char *p;

	p = (str + strlen(str));
	while(isspace(*--p)) {
		*p = '\0';
	}

	return(p);
}

/*
 * addr_parse() -- Parse <user@domain.com> from full email address
 */
static char *
addr_parse(char *str)
{
	char *p, *q;

#if 0
	(void)fprintf(stderr, "*** addr_parse(): str = [%s]\n", str);
#endif

	/* Simple case with email address enclosed in <> */
	if((p = strdup(str)) == (char *)NULL) {
		die("addr_parse(): strdup()");
	}

	if((q = strchr(p, '<'))) {
		q++;

		if((p = strchr(q, '>'))) {
			*p = '\0';
		}

#if 0
		(void)fprintf(stderr, "*** addr_parse(): q = [%s]\n", q);
#endif

		return(q);
	}

	q = strip_pre_ws(p);
	if(*q == '(') {
		while((*q++ != ')'));
	}
	p = strip_pre_ws(q);

#if 0
	(void)fprintf(stderr, "*** addr_parse(): p = [%s]\n", p);
#endif

	q = strip_post_ws(p);
	if(*q == ')') {
		while((*--q != '('));
		*q = '\0';
	}
	(void)strip_post_ws(p);

#if 0
	(void)fprintf(stderr, "*** addr_parse(): p = [%s]\n", p);
#endif

	return(p);
}

/*
 * append_domain() -- Fix up address with @domain.com
 */
static char *
append_domain(char *str)
{
	char buf[(BUF_SZ + 1)];

	if(strchr(str, '@') == (char *)NULL) {
		int ret;

#ifdef REWRITE_DOMAIN
		ret = snprintf(buf, BUF_SZ, "%s@%s", str,
			rewrite_domain == True ? mail_domain : hostname);
#else
		ret = snprintf(buf, BUF_SZ, "%s@%s", str, hostname);
#endif
		if (ret == -1)
			die("append_domain() -- snprintf() failed");

		return(strdup(buf));
	}

	return(strdup(str));
}

/*
 * standardise() -- Trim off '\n's and double leading dots
 */
static bool_t
standardise(char *str, bool_t *linestart)
{
	char *p;
	bool_t leadingdot = False;

	/*
	 * Any line beginning with a dot has an additional dot inserted;
	 * not just a line consisting solely of a dot. Thus we have to move
	 * the buffer start up one
	 */

	if(*linestart && *str == '.') {
		leadingdot = True;
	}
	*linestart = False;

	if((p = strchr(str, '\n'))) {
		*p = '\0';
		*linestart = True;
	}
	return(leadingdot);
}

/*
 * revaliases() -- Parse the reverse alias file
 * 	Fix globals to use any entry for sender
 */
static void
revaliases(struct passwd *pw)
{
	char buf[(BUF_SZ + 1)], *p;
	FILE *fp;

	/* Try to open the reverse aliases file */
	if((fp = fopen(REVALIASES_FILE, "r"))) {
		/* Search if a reverse alias is defined for the sender */
		while(fgets(buf, sizeof(buf), fp)) {
			/* Make comments invisible */
			if((p = strchr(buf, '#'))) {
				*p = '\0';
			}

			/* Ignore malformed lines and comments */
			if(strchr(buf, ':') == (char *)NULL) {
				continue;
			}

			/* Parse the alias */
			if(((p = strtok(buf, ":"))) && !strcmp(p, pw->pw_name))
			{
				if((p = strtok(NULL, ": \t\r\n"))) {
					if((uad = strdup(p)) == (char *)NULL) {
						die("revaliases() -- strdup()"
							       " failed");
					}
				}

				if((p = strtok(NULL, " \t\r\n:"))) {
					if((mailhost.name = strdup(p)) ==
							(char *)NULL) {
						die("revaliases() -- strdup() "
								"failed");
					}

					if((p = strtok(NULL, " \t\r\n:"))) {
						mailhost.port = atoi(p);
					}

					if(log_level > 0) {
						log_event(LOG_INFO,
							"Set MailHub=\"%s\"\n",
							mailhost.name);
						log_event(LOG_INFO,
							"via SMTP Port Number="
							"\"%d\"\n",
							mailhost.port);
					}
				}
			}
		}

		fclose(fp);
	}
}

/* 
 * from_strip() -- Transforms "Name <login@host>" into "login@host" or
 * "login@host (Real name)"
 */
static char *
from_strip(char *str)
{
	char *p;

#if 0
	(void)fprintf(stderr, "*** from_strip(): str = [%s]\n", str);
#endif

	if(strncmp("From:", str, 5) == 0) {
		str += 5;
	}

	/* Remove the real name if necessary - just send the address */
	if((p = addr_parse(str)) == (char *)NULL) {
		die("from_strip() -- addr_parse() failed");
	}
#if 0
	(void)fprintf(stderr, "*** from_strip(): p = [%s]\n", p);
#endif

	return(strdup(p));
}

/*
 * from_format() -- Generate standard From: line
 */
static char *
from_format(char *str, bool_t override_from)
{
	char buf[(BUF_SZ + 1)] = "";

	if(override_from) {
		if(minus_f) {
			str = append_domain(minus_f);
		}

		if(minus_F) {
			if(snprintf(buf,
				BUF_SZ, "\"%s\" <%s>", minus_F, str) == -1) {
				die("from_format() -- snprintf() failed");
			}
		}
		else if(gecos) {
			if(snprintf(buf, BUF_SZ, "\"%s\" <%s>", gecos, str) ==
					-1) {
				die("from_format() -- snprintf() failed");
			}
		}
		else {
			if(snprintf(buf, BUF_SZ, "%s", str) == -1) {
				die("from_format() -- snprintf() failed");
			}
		}
	}
	else {
		if(gecos) {
			if(snprintf(buf, BUF_SZ, "\"%s\" <%s>", gecos, str) ==
					-1) {
				die("from_format() -- snprintf() failed");
			}
		}
		else {
			if(snprintf(buf, BUF_SZ, "%s", str) == -1) {
				die("from_format() -- snprintf() failed");
			}
		}
	}

#if 0
	(void)fprintf(stderr, "*** from_format(): buf = [%s]\n", buf);
#endif

	return(strdup(buf));
}

/*
 * rcpt_save() -- Store entry into RCPT list
 */
static void
rcpt_save(char *str, struct list_head *recips)
{
	char			*p;
	struct string_node	*node;

# if 1
	/* Horrible botch for group stuff */
	p = str;
	while(*p) p++;

	if(*--p == ';') {
		return;
	}
#endif

#if 0
	(void)fprintf(stderr, "*** rcpt_save(): str = [%s]\n", str);
#endif

	/* Ignore missing usernames */
	if(*str == '\0') {
		return;
	}

	if ((node = malloc(sizeof(*node))) == NULL)
		die("rcpt_save() -- malloc() failed");

	if((node->string = strdup(str)) == (char *)NULL) {
		die("rcpt_save() -- strdup() failed");
	}

	list_add_tail(recips, &node->list);
}

/*
 * rcpt_parse() -- Break To|Cc|Bcc into individual addresses
 */
static void
rcpt_parse(char *str, struct list_head *recips)
{
	bool_t in_quotes = False, got_addr = False;
	char *p, *q, *r;
	struct string_node *node;

#if 0
	(void)fprintf(stderr, "*** rcpt_parse(): str = [%s]\n", str);
#endif

	if((p = strdup(str)) == (char *)NULL) {
		die("rcpt_parse(): strdup() failed");
	}
	q = p;

	/* Replace <CR>, <LF> and <TAB> */
	while(*q) {
		switch(*q) {
			case '\t':
			case '\n':
			case '\r':
					*q = ' ';
		}
		q++;
	}
	q = p;

#if 0
	(void)fprintf(stderr, "*** rcpt_parse(): q = [%s]\n", q);
#endif

	r = q;
	while(*q) {
		if(*q == '"') {
			in_quotes = (in_quotes ? False : True);
		}

		/* End of string? */
		if(*(q + 1) == '\0') {
			got_addr = True;
		}

		/* End of address? */
		if((*q == ',') && (in_quotes == False)) {
			got_addr = True;

			*q = '\0';
		}

		if(got_addr) {
			while(*r && isspace(*r)) r++;

			rcpt_save(addr_parse(r), recips);
			r = (q + 1);
#if 0
			(void)fprintf(stderr, "*** rcpt_parse(): r = [%s]\n", r);
#endif
			got_addr = False;
		}
		q++;
	}
	free(p);
}

#ifdef MD5AUTH
static int
crammd5(
	char	*challengeb64,
	char	*username,
	char	*password,
	char	*responseb64,
	int	responselen)
{
	unsigned int i;
	unsigned char digest[MD5_DIGEST_LEN];
	unsigned char digascii[MD5_DIGEST_LEN * 2 + 1];
	unsigned char challenge[(BUF_SZ + 1)];
	unsigned char response[(BUF_SZ + 1)];
	unsigned char secret[(MD5_BLOCK_LEN + 1)];

	memset (secret,0,sizeof(secret));
	memset (challenge,0,sizeof(challenge));
	strncpy ((char *)secret, password, sizeof(secret));
	if (!challengeb64 || strlen(challengeb64) > sizeof(challenge) * 3 / 4)
		return 0;
	B64DEC(challengeb64, strlen(challengeb64), challenge, BUF_SZ, &i);

	hmac_md5((unsigned char *)challenge, strlen(challenge),
			(unsigned char *)secret, strlen(secret), digest);

	for (i = 0; i < MD5_DIGEST_LEN; i++) {
		digascii[2 * i] = hextab[digest[i] >> 4];
		digascii[2 * i + 1] = hextab[(digest[i] & 0x0F)];
	}
	digascii[MD5_DIGEST_LEN * 2] = '\0';

	if (sizeof(response) <= strlen(username) + sizeof(digascii))
		return 0;

	strncpy (response, username, sizeof(response) - sizeof(digascii) - 2);
	strcat (response, " ");
	strcat (response, (char *)digascii);
	B64ENC(response, strlen(response), responseb64, responselen, NULL);

	return 1;
}
#endif

/*
 * rcpt_remap() -- Alias systems-level users to the person who
 *	reads their mail. This is variously the owner of a workstation,
 * 	the sysadmin of a group of stations and the postmaster otherwise.
 *	We don't just mail stuff off to root on the mailhub :-)
 */
static char *
rcpt_remap(char *str)
{
	struct passwd *pw;
	if((root==NULL) || strlen(root)==0 || strchr(str, '@') ||
		((pw = getpwnam(str)) == NULL) || (pw->pw_uid >= minuserid)) {
		return(append_domain(str));	/* It's not a local systems-level user */
	}
	else {
		return(append_domain(root));
	}
}

/*
 * header_save() -- Store entry into header list
 */
static void
header_save(
	const char		*str,
	struct list_head	*headers,
	struct list_head	*recips,
	bool_t			minus_t_set)
{
	char			*p;
	struct string_node	*node;


#if 0
	(void)fprintf(stderr, "header_save(): str = [%s]\n", str);
#endif
	if((p = strdup(str)) == (char *)NULL) {
		die("header_save() -- strdup() failed");
	}

	if ((node = malloc(sizeof(*node))) == NULL) {
		die("header_save() -- malloc() failed");
	}

	node->string = p;
	list_add_tail(headers, &node->list);

	if(strncasecmp(node->string, "From:", 5) == 0) {
#if 1
		/* Hack check for NULL From: line */
		if(*(p + 6) == '\0') {
			return;
		}
#endif

#ifdef REWRITE_DOMAIN
		if(override_from == True) {
			uad = from_strip(node->string);
		}
		else {
			return;
		}
#endif
		have_from = True;
	}
#ifdef HASTO_OPTION
	else if(strncasecmp(node->string, "To:" ,3) == 0) {
		have_to = True;
	}
#endif
	else if(strncasecmp(node->string, "Date:", 5) == 0) {
		have_date = True;
	}

	if(minus_t_set) {
		/* Need to figure out recipients from the e-mail */
		if(strncasecmp(node->string, "To:", 3) == 0) {
			p = (node->string + 3);
			rcpt_parse(p, recips);
		}
		else if(strncasecmp(node->string, "Bcc:", 4) == 0) {
			p = (node->string + 4);
			rcpt_parse(p, recips);
                        /* Undo adding the header to the list: */
                        free(node->string);
                        node->string = NULL;
			list_del(&node->list);
                        return;
		}
		else if(strncasecmp(node->string, "CC:", 3) == 0) {
			p = (node->string + 3);
			rcpt_parse(p, recips);
		}
	}

#if 0
	(void)fprintf(stderr, "header_save(): node->string = [%s]\n", node->string);
#endif
}

/*
 * header_parse() -- Break headers into seperate entries
 */
void
header_parse(
	int			fd,
	struct list_head	*headers,
	struct list_head	*recips,
	bool_t			minus_t_set)
{
	size_t size = BUF_SZ, len = 0;
	char *p = (char *)NULL, *q = NULL;
	bool_t in_header = True;
	char l = '\0';
	int c;

	while(in_header && read(fd, &c, 1) > 0) {
		/* Must have space for up to two more characters, since we
			may need to insert a '\r' */
		if((p == (char *)NULL) || (len >= (size - 1))) {
			size += BUF_SZ;

			p = (char *)realloc(p, (size * sizeof(char)));
			if(p == (char *)NULL) {
				die("header_parse() -- realloc() failed");
			}
			q = (p + len);
		}
		len++;

		if(l == '\n') {
			switch(c) {
			case ' ':
			case '\t':
				/* Must insert '\r' before '\n's embedded in
				 * header fields otherwise qmail won't accept
				 * our mail because a bare '\n' violates some 
				 * RFC
				 */

				/* Replace previous \n with \r */
				*(q - 1) = '\r';
				/* Insert \n */
				*q++ = '\n';
				len++;
				break;

			case '\n':
				in_header = False;

			default:
				*q = 0;
				if((q = strrchr(p, '\n'))) {
					*q = 0;
				}
				header_save(p, headers, recips, minus_t_set);

				q = p;
				len = 0;
			}
		}
		*q++ = c;

		l = c;
	}
	if(in_header) {
		if(l == '\n') {
			switch(c) {
			case ' ':
			case '\t':
				/*
				 * Must insert '\r' before '\n's embedded in 
				 * header fields otherwise qmail won't accept 
				 * our mail because a bare '\n' violates some 
				 * RFC
				 */
				/* Replace previous \n with \r */
				*(q - 1) = '\r';
				/* Insert \n */
				*q++ = '\n';
				len++;
				break;

			case '\n':
				in_header = False;

			default:
				*q = 0;
				if((q = strrchr(p, '\n'))) {
					*q = 0;
				}
				header_save(p, headers, recips, minus_t_set);

				q = p;
				len = 0;
			}
		}
	}
	(void)free(p);
}

static void
add_config(
	const char		*left,
	const char		*right,
	struct list_head	*headers,
	struct list_head	*recips)
{
	enum type {
		HOSTPORT,
		STRING,
		INTEGER,
		BOOLEAN,
		FUNCTION_HDR
	};

	typedef void (*conf_func_t)(const char *);

	struct conf_var {
		char		*name;
		void		*location;
		enum type	type;
	};

	/* save us from nasty casting cur->location all over the place */
	union {
		struct hostport	*h;
		char		**s;
		int		*i;
		bool_t		*b;
	} u;

	struct conf_var *cur;
	struct conf_var conf_vars[] = {
		{ "Root",		&root,		STRING		},
		{ "MailHub",		&mailhost,	HOSTPORT	},
		{ "HostName",		&hostname,	STRING		},
		{ "AddHeader",		NULL,		FUNCTION_HDR	},
		{ "FromLineOverride",	&override_from,	BOOLEAN		},
		{ "RemotePort",		&mailhost.port,	INTEGER		},
		{ "AuthUser",		&auth_user,	STRING		},
		{ "AuthPass",		&auth_pass,	STRING		},
		{ "AuthMethod",		&auth_method,	STRING		},
		{ "UseOldAuth",		&use_oldauth,	BOOLEAN		},
		{ "Debug",		&log_level,	BOOLEAN		},
		{ "MinUserId",		&minuserid,	INTEGER		},
#ifdef REWRITE_DOMAIN
		{ "RewriteDomain",	&mail_domain,	STRING		},
#endif
#ifdef HAVE_SSL
		{ "UseTLS",		&use_tls,	BOOLEAN		},
		{ "UseSTARTTLS",	&use_starttls,	BOOLEAN		},
		{ "UseTLSCert",		&use_cert,	BOOLEAN		},
		{ "TLSCert",		&tls_cert,	STRING		},
#endif
		{ NULL,			NULL,		0		}
	};

	for (cur = conf_vars; cur->name; cur++) {
		if (!strcasecmp(cur->name, left))
			break;
	}

	if (!cur->name)
		return;

	u.s = cur->location;

	switch (cur->type) {
	case HOSTPORT: {
		char *v;
		char *p;

		v = strdup(right);
		if ((p = strchr(v, ':'))) {
			u.h->port = atoi(p+1);
			*p = 0;

			if (log_level)
				log_event(LOG_INFO, "Set %s.port=\"%d\"\n",
							cur->name, u.h->port);
		}

		u.h->name = strdup(v);
		free(v);

		if (log_level)
			log_event(LOG_INFO, "Set %s.name=\"%s\"\n",
							cur->name, u.h->name);
		} break;
	case STRING:
		*u.s = strdup(right);
		if (log_level)
			log_event(LOG_INFO, "Set %s=\"%s\"\n", cur->name, *u.s);

		break;
	case INTEGER:
		*u.i = atoi(right);
		if (log_level)
			log_event(LOG_INFO, "Set %s=\"%d\"\n", cur->name, *u.i);
		break;
	case BOOLEAN:
		*u.b = strcasecmp(right, "YES") ? False : True;
		if (log_level)
			log_event(LOG_INFO, "Set %s=\"%d\"\n", cur->name, *u.b);
		break;
	case FUNCTION_HDR:
		header_save(right, headers, recips, minus_t);
		break;
	}
}

/*
 * read_config() -- Open and parse config file and extract values of variables
 */
static bool_t
read_config()
{
	int	ret;
	FILE	*fp;
	char	buf[BUF_SZ];
	regex_t re;

	if (!config_file)
		config_file = CONFIGURATION_FILE;

	if(!(fp = fopen(config_file, "r"))) {
		log_event(LOG_ERR, "fopen %s: %s\n", config_file,
							strerror(errno));
		return False;
	}

	ret = regcomp(&re, "^[[:space:]]*([[:alnum:]]+)[[:space:]]*="
		"[[:space:]]*([[:alnum:]].*)$", REG_EXTENDED|REG_NEWLINE);
	if (ret) {
		char errbuf[64];
		regerror(ret, &re, errbuf, sizeof(errbuf));
		log_event(LOG_ERR, "regcomp failed: %s\n", errbuf);
		fclose(fp);
		return False;
	}

	while(fgets(buf, sizeof(buf), fp)) {
		char		*left;
		char		*right;
		regmatch_t	pmatch[3];

		memset(pmatch, 0, sizeof(pmatch));
		ret = regexec(&re, buf, ARRAY_SIZE(pmatch), pmatch, 0);

		if (ret == REG_NOMATCH)
			continue;

		if (ret) {
			char errbuf[64];
			regerror(ret, &re, errbuf, sizeof(errbuf));
			log_event(LOG_ERR, "regexec: %s\n", errbuf);
			continue;
		}

		buf[pmatch[1].rm_eo] = 0;
		left = &buf[pmatch[1].rm_so];

		buf[pmatch[2].rm_eo] = 0;
		right = &buf[pmatch[2].rm_so];

		add_config(left, right, &header_list, &rcpt_list);
	}
	regfree(&re);
	fclose(fp);

	/* STARTTLS implies TLS */
	if (use_starttls == True)
		use_tls = True;

	return (True);
}

/*
 * fd_getc() -- Read a character from an fd
 */
static ssize_t
fd_getc(int fd, void *c)
{
#ifdef HAVE_SSL
	if(use_tls == True) {
		return(SSL_read(ssl, c, 1));
	}
#endif
	return(read(fd, c, 1));
}

/*
 * fd_gets() -- Get characters from a fd instead of an fp
 */
static char *
fd_gets(char *buf, int size, int fd)
{
	int i = 0;
	char c;

	while((i < size) && (fd_getc(fd, &c) == 1)) {
		if(c == '\r');	/* Strip <CR> */
		else if(c == '\n') {
			break;
		}
		else {
			buf[i++] = c;
		}
	}
	buf[i] = 0;

	return(buf);
}

/*
 * fd_puts() -- Write characters to fd
 */
static ssize_t
fd_puts(int fd, const void *buf, size_t count)
{
#ifdef HAVE_SSL
	if(use_tls == True) {
		return(SSL_write(ssl, buf, count));
	}
#endif
	return(write(fd, buf, count));
}

/*
 * smtp_write() -- A printf to an fd and append <CR/LF>
 */
static ssize_t
smtp_write(int fd, char *format, ...)
{
	char buf[(BUF_SZ + 2)];
	va_list ap;
	ssize_t outbytes = 0;

	va_start(ap, format);
	if(vsnprintf(buf, (BUF_SZ - 1), format, ap) == -1) {
		die("smtp_write() -- vsnprintf() failed");
	}
	va_end(ap);

	if(log_level > 0) {
		log_event(LOG_INFO, "%s\n", buf);
	}

	if(minus_v) {
		(void)fprintf(stderr, "[->] %s\n", buf);
	}
	(void)strcat(buf, "\r\n");

	outbytes = fd_puts(fd, buf, strlen(buf));

	return (outbytes >= 0) ? outbytes : 0;
}

/*
 * smtp_read() -- Get a line and return the initial digit
 */
static int
smtp_read(int fd, char *response)
{
	do {
		if(fd_gets(response, BUF_SZ, fd) == NULL) {
			return(0);
		}

		if (strstr(response, "AUTH")) {
			auth_method = strdup(&response[9]);
		}
	}
	while(response[3] == '-');

	if(log_level > 0) {
		log_event(LOG_INFO, "%s\n", response);
	}

	if(minus_v) {
		(void)fprintf(stderr, "[<-] %s\n", response);
	}

	return(atoi(response) / 100);
}

/*
 * smtp_okay() -- Get a line and test the three-number string at the beginning
 *				If it starts with a 2, it's OK
 */
static int
smtp_okay(int fd, char *response)
{
	return((smtp_read(fd, response) == 2) ? 1 : 0);
}


/*
 * smtp_open() -- Open connection to a remote SMTP listener
 */
static int
smtp_open(char *host, int port)
{
#ifdef INET6
	struct addrinfo hints, *ai0, *ai;
	char servname[NI_MAXSERV];
	int s;
#else
	struct sockaddr_in name;
	struct hostent *hent;
	int i, s, namelen;
#endif

#ifdef HAVE_SSL
	int err;
	char buf[(BUF_SZ + 1)];

	/* Special handling of const to avoid compile warnings */
#ifdef HAVE_GNUTLS
	const X509 *server_cert;
	SSL_METHOD *meth;
#else
	X509 *server_cert;
	const SSL_METHOD *meth;
#endif

	/* Init SSL stuff */
	SSL_CTX *ctx;

	SSL_load_error_strings();
	SSLeay_add_ssl_algorithms();
	meth = SSLv23_client_method();
	ctx = SSL_CTX_new(meth);
	if(!ctx) {
		log_event(LOG_ERR, "No SSL support initiated\n");
		return(-1);
	}

	if(use_cert == True) {
#ifdef HAVE_GNUTLS
		if(SSL_CTX_use_certificate_file(ctx, tls_cert, SSL_FILETYPE_PEM) <= 0) {
#else
		if(SSL_CTX_use_certificate_chain_file(ctx, tls_cert) <= 0) {
#endif
			perror("Use certfile");
			return(-1);
		}

		if(SSL_CTX_use_PrivateKey_file(ctx, tls_cert, SSL_FILETYPE_PEM)
				<= 0) {
			perror("Use PrivateKey");
			return(-1);
		}

#ifndef HAVE_GNUTLS
		if(!SSL_CTX_check_private_key(ctx)) {
			log_event(LOG_ERR, "Private key does not match the "
					"certificate public key\n");
			return(-1);
		}
#endif
	}
#endif

#ifdef INET6
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = p_family;
	hints.ai_socktype = SOCK_STREAM;
	snprintf(servname, sizeof(servname), "%d", port);

	/* Check we can reach the host */
	if (getaddrinfo(host, servname, &hints, &ai0)) {
		log_event(LOG_ERR, "Unable to locate %s: %s", host, strerror(errno));
		return(-1);
	}

	for (ai = ai0; ai; ai = ai->ai_next) {
		/* Create a socket for the connection */
		s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (s < 0) {
			continue;
		}

		if (connect(s, ai->ai_addr, ai->ai_addrlen) < 0) {
			close(s);
			s = -1;
			continue;
		}
		break;
	}

	if(s < 0) {
		log_event (LOG_ERR,
			"Unable to connect to \"%s\" port %d: \n", host, port, strerror(errno));

		return(-1);
	}
#else
	/* Check we can reach the host */
	if((hent = gethostbyname(host)) == (struct hostent *)NULL) {
		log_event(LOG_ERR, "Unable to resolve %s: %s", host, strerror(errno));
		return(-1);
	}

	if((unsigned)hent->h_length > sizeof(hent->h_addr)) {
		log_event(LOG_ERR, "Buffer overflow in gethostbyname()");
		return(-1);
	}

	/* Create a socket for the connection */
	if((s = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0) {
		log_event(LOG_ERR, "Unable to create a socket: %s", strerror(errno));
		return(-1);
	}

	for (i = 0; ; ++i) {
		if (!hent->h_addr_list[i]) {
			log_event(LOG_ERR, "Unable to connect to %s port %d: %s", host,
					port, strerror(errno));
			return(-1);
		}

	/* This SHOULD already be in Network Byte Order from gethostbyname() */
	name.sin_addr.s_addr =
		((struct in_addr *)(hent->h_addr_list[i]))->s_addr;
	name.sin_family = hent->h_addrtype;
	name.sin_port = htons(port);

	namelen = sizeof(struct sockaddr_in);
	if(connect(s, (struct sockaddr *)&name, namelen) < 0)
		continue;
	break;
	}
#endif

#ifdef HAVE_SSL
	if(use_tls == True) {
		log_event(LOG_INFO, "Creating SSL connection to host");

		if (use_starttls == True)
		{
			/* need to write plain text for a while */
			use_tls=False;

			if (smtp_okay(s, buf))
			{
				smtp_write(s, "EHLO %s", hostname);
				if (smtp_okay(s, buf)) {
					/* assume STARTTLS regardless */
					smtp_write(s, "STARTTLS");
					if (!smtp_okay(s, buf)) {
						log_event(LOG_ERR, "STARTTLS "
								"not working");
						return(-1);
					}
				}
				else
				{
					log_event(LOG_ERR, "Invalid response: "
						"%s (%s)", buf, hostname);
				}
			}
			else
			{
				log_event(LOG_ERR, "Invalid response SMTP "
						"Server (STARTTLS)");
				return(-1);
			}
			/* now continue as normal for SSL */
			use_tls=True;
		}

		ssl = SSL_new(ctx);
		if(!ssl) {
			log_event(LOG_ERR, "SSL not working");
			return(-1);
		}
		SSL_set_fd(ssl, s);

		err = SSL_connect(ssl);
		if(err < 0) {
			perror("SSL_connect");
			return(-1);
		}

		if(log_level > 0 || 1) {
			log_event(LOG_INFO, "SSL connection using %s",
				SSL_get_cipher(ssl));
		}

		server_cert = SSL_get_peer_certificate(ssl);
		if(!server_cert) {
			return(-1);
		}
		X509_free(server_cert);

		/* TODO: Check server cert if changed! */
	}
#endif

	return(s);
}

#ifdef HAVE_SASL
static int
get_user(
	void		*context __attribute__((__unused__)),
	int		id,
	const char	**result,
	unsigned	*len)
{
	if (log_level) {
		log_event(LOG_INFO, "get_user: id %d", id);
	}

	if (id != SASL_CB_USER && id != SASL_CB_AUTHNAME) {
		return SASL_BADPARAM;
	}

	*result = strdup(auth_user);
	if (!result)
		return SASL_NOMEM;

	if (len)
		*len = strlen(*result);

	if (log_level) {
		log_event(LOG_INFO, "get_user: %s %d%s", *result,
				len ? *len : 0, len ? "" : " (null)");
	}

	return SASL_OK;
}

static int get_realm(
		void		*context __attribute__((__unused__)),
		int		id __attribute__((__unused__)),
		const char	**availrealms __attribute__ ((unused)),
		const char	**result)
{
	log_event(LOG_INFO, "get_realm");
	*result = "";
	return SASL_OK;
}

static int get_secret(
		sasl_conn_t	*conn __attribute__((__unused__)),
		void		*context __attribute__((__unused__)),
		int		id __attribute__((__unused__)),
		sasl_secret_t	**psecret)
{
	unsigned int len = (unsigned) strlen(auth_pass);

	log_event(LOG_INFO, "get_secret");
	*psecret = malloc(sizeof(sasl_secret_t) + len);
	(*psecret)->len = len;
	strcpy((char *)(*psecret)->data, auth_pass);

	if (log_level) {
		log_event(LOG_INFO, "get_secret: password is '%s'", auth_pass);
	}

	return SASL_OK;
}

static int
log_for_sasl(
	void		*context __attribute__((__unused__)),
	int		level,
	const char	*message)
{
	log_event(level, "SASL: %s", message);

	return SASL_OK;
}
#endif

/*
 * handler() -- A "normal" non-portable version of an alarm handler
 * Alas, setting a flag and returning is not fully functional in BSD: system
 * calls don't fail when reading from a ``slow'' device like a socket. So we
 * longjump instead, which is erronious on a small number of machines and
 * ill-defined in the language
 */
static void
handler(void)
{
	extern jmp_buf TimeoutJmpBuf;

	longjmp(TimeoutJmpBuf, (int)1);
}

/*
 * ssmtp() -- send the message (exactly one) from stdin to the mailhub SMTP port
 */
int
ssmtp(char **argv)
{
	char b[(BUF_SZ + 2)], *buf = b+1, *p, *q;
	char *remote_addr;
#if defined MD5AUTH
	char challenge[(BUF_SZ + 1)];
#endif
	struct passwd *pw;
	int i, sock;
	uid_t uid;
	bool_t minus_v_save, leadingdot, linestart = True;
	int timeout = 0;
	int bufsize = sizeof(b)-1;
	char tmpbuf[257];
	FILE *input = NULL;
	struct string_node *node;

	hostname = xgethostname();
	if(!hostname) {
		perror("xgethostname");
		die("Cannot get the name of this machine");
	}

	/* create temporary file in which we're going to store stdin */
	input = tmpfile();

	/* do we have tmpfile? */
	if (input == NULL) {
		log_event(
			LOG_ERR,
			"Unable to create temporary file for stdin storage: %s; falling back to direct stdin usage.",
			strerror(errno)
		);
		input = stdin;
	} else {
		/* Add X-Start-Date to message */
		fprintf(input, "X-Start-Date: %s\r\n", get_arpadate_now());

		/* read from stdin, write to tmpfile */
		while (fgets(tmpbuf, sizeof(tmpbuf), stdin) != NULL) {
			if (fputs(tmpbuf, input) == EOF) {
				int err = errno;
				log_event(
					LOG_ERR,
					"Unable to write stdin buffer [%d of max %d bytes] to temporary file: %s\n",
					strlen(tmpbuf),
					sizeof(tmpbuf),
					strerror(err)
				);
			}
		}

		fsync(fileno(input));

		/* rewind tmpfile back to beginning... */
		/* we'll use tmpf later instead of stdin... */
		rewind(input);

		/* store tmp fd to global too... */
		/* maybe we'll have to write a dead.letter */
		input_tmpf = input;
	}

	b[0] = '.';
	outbytes = 0;

	uid = getuid();
	if((pw = getpwuid(uid)) == (struct passwd *)NULL) {
		die("Could not find password entry for UID %d", uid);
	}

	if(read_config() == False) {
		log_event(LOG_INFO, "%s not found", config_file);
	}

	if((p = strtok(pw->pw_gecos, ";,"))) {
		if((gecos = strdup(p)) == (char *)NULL) {
			die("ssmtp() -- strdup() failed");
		}
	}
	revaliases(pw);

	/* revaliases() may have defined this */
	if(uad == (char *)NULL) {
		uad = append_domain(pw->pw_name);
	}

	header_parse(fileno(input), &header_list, &rcpt_list, minus_t);

#if 1
	/*
	 * With FromLineOverride=YES set, try to recover sane MAIL FROM
	 * address
	 */
	uad = append_domain(uad);
#endif

	from = from_format(uad, override_from);

	/* Now to the delivery of the message */
	(void)signal(SIGALRM, (void(*)())handler);	/* Catch SIGALRM */
	(void)alarm((unsigned) MAXWAIT);		/* Set initial timer */
	if(setjmp(TimeoutJmpBuf) != 0) {
		/* Then the timer has gone off and we bail out */
		die("Connection lost in middle of processing");
	}

	if((sock = smtp_open(mailhost.name, mailhost.port)) == -1) {
		die("Cannot open %s:%d", mailhost.name, mailhost.port);
	}
	else if (use_starttls == False) /* no initial response after STARTTLS */
	{
		if(smtp_okay(sock, buf) == False)
			die("Invalid response SMTP server");
	}

	/* If user supplied username and password, then try ELHO */
	if(auth_user) {
		outbytes += smtp_write(sock, "EHLO %s", hostname);
	}
	else {
		outbytes += smtp_write(sock, "HELO %s", hostname);
	}
	(void)alarm((unsigned) MEDWAIT);

	if(smtp_okay(sock, buf) == False) {
		die("%s (%s)", buf, hostname);
	}

	/* Try to log in if username was supplied */
	if(auth_user) {
#ifdef HAVE_SASL
		sasl_conn_t	*pconn;
		const char	*mech = NULL;
		char		*clientout;
		int		ret, pcount;
		unsigned int	clientoutlen;

		sasl_callback_t	cb[] = {
			{ SASL_CB_LOG,		log_for_sasl,	NULL },
			{ SASL_CB_GETREALM,	get_realm,	NULL },
			{ SASL_CB_USER,		get_user,	NULL },
			{ SASL_CB_AUTHNAME,	get_user,	NULL },
			{ SASL_CB_PASS,		get_secret,	NULL },
			{ SASL_CB_LIST_END,	NULL,		NULL }
		};

		ret = sasl_client_init(cb);
		if (ret != SASL_OK) {
			die("sasl_client_init: SASL init failed %d", ret);
		}

		ret = sasl_client_new("smtp", mailhost.name, NULL, NULL, NULL,
								0, &pconn);
		if (ret != SASL_OK) {
			die("sasl_client_new: SASL setup failed %d", ret);
		}

		if (log_level) {
			char		*client_mechs;
			unsigned	len;

			log_event(LOG_INFO, "Server mechanisms '%s' %s",
						auth_method, mailhost.name);
			sasl_listmech(pconn, NULL, "", ",", "",
						(const char **)&client_mechs,
						&len, &pcount);
			log_event(LOG_INFO, "Client mechanisms '%s'",
						client_mechs);
		}

		ret = sasl_client_start(pconn, auth_method, NULL,
			(const char **)&clientout, &clientoutlen, &mech);
		if (log_level) {
			log_event(LOG_INFO, "start client output is '%s' %d "
					"(%s)", clientout, clientoutlen,
					sasl_errstring(ret, NULL, NULL));
		}
		if (!(ret == SASL_OK || ret == SASL_CONTINUE)) {
			die("sasl_client_start: failed %d", ret);
		}

		if (log_level) {
			log_event(LOG_INFO, "Selected mechanism: '%s'", mech);
		}

		sprintf(buf, "AUTH %s", mech);
		if (clientoutlen) {
			ret = strlen(buf);
			buf[ret++] = ' ';
			B64ENC(clientout, clientoutlen, &buf[ret],
						BUF_SZ - ret, NULL);
		}

		do {
			char		buf_decode[1024];
			unsigned	blen = 0;

			memset(buf_decode, 0, sizeof(buf_decode));

			smtp_write(sock, "%s", buf);

			memset(buf, 0, BUF_SZ);
			if (smtp_read(sock, buf) == 2) {
				memset(buf, 0, BUF_SZ);
				goto finished;
			}

			B64DEC(&buf[4], strlen(buf) - 4, buf_decode,
						sizeof(buf_decode), &blen);
			if (log_level) {
				log_event(LOG_INFO, "decoded '%s' as '%s' (%d)",
							buf, buf_decode, blen);
			}

			ret = sasl_client_step(pconn, buf_decode, blen, NULL,
				(const char **)&clientout, &clientoutlen);

			if (log_level) {
				log_event(LOG_INFO, "response %d: %d '%s'",
						ret, clientoutlen, clientout);
			}

			B64ENC(clientout, clientoutlen, buf, BUF_SZ, &blen);
		} while (ret == SASL_CONTINUE);

		if (ret != SASL_OK) {
			log_event(LOG_INFO, "%s", sasl_errdetail(pconn));
			die("SASL failed: %s", sasl_errstring(ret, NULL, NULL));
		}

		goto authorised;

#elif defined MD5AUTH
		if(auth_pass == (char *)NULL) {
			auth_pass = strdup("");
		}

		if(auth_method && strcasecmp(auth_method, "cram-md5") == 0) {
			outbytes += smtp_write(sock, "AUTH CRAM-MD5");
			(void)alarm((unsigned) MEDWAIT);

			if(smtp_read(sock, buf) != 3) {
				die("Server rejected AUTH CRAM-MD5 (%s)", buf);
			}
			strncpy(challenge, strchr(buf,' ') + 1,
					sizeof(challenge));

			memset(buf, 0, bufsize);
			crammd5(challenge, auth_user, auth_pass, buf, bufsize);
			goto authorised;
		}
#endif
		if (use_oldauth) {
			memset(buf, 0, bufsize);
			B64ENC(auth_user, strlen(auth_user), buf, bufsize,
								NULL);
			outbytes += smtp_write(sock, "AUTH LOGIN %s", buf);
		}
		else {
			outbytes += smtp_write(sock, "AUTH LOGIN");
			(void)alarm((unsigned) MEDWAIT);
			if(smtp_read(sock, buf) != 3) {
				die("Server didn't like our AUTH LOGIN (%s)",
						buf);
			}
			/* we assume server asked us for Username */
			memset(buf, 0, bufsize);
			B64ENC(auth_user, strlen(auth_user), buf, bufsize,
								NULL);
			outbytes += smtp_write(sock, buf);
		}

		(void)alarm((unsigned) MEDWAIT);
		if(smtp_read(sock, buf) != 3) {
			die("Server didn't accept AUTH LOGIN (%s)", buf);
		}
		memset(buf, 0, bufsize);

		B64ENC(auth_pass, strlen(auth_pass), buf, bufsize, NULL);

#if defined(HAVE_SASL) || defined(MD5AUTH)
authorised:
#endif
		/* We do NOT want the password output to STDERR
		 * even base64 encoded.*/
		minus_v_save = minus_v;
		minus_v = False;
		outbytes += smtp_write(sock, "%s", buf);
		minus_v = minus_v_save;
		(void)alarm((unsigned) MEDWAIT);

		if(smtp_okay(sock, buf) == False) {
			die("Authorization failed (%s)", buf);
		}

#ifdef HAVE_SASL
finished:
		sasl_dispose(&pconn);
		sasl_done();
#endif
	}

	/* Send "MAIL FROM:" line */
	outbytes += smtp_write(sock, "MAIL FROM:<%s>", uad);

	(void)alarm((unsigned) MEDWAIT);

	if(smtp_okay(sock, buf) == 0) {
		die("%s", buf);
	}

	/* Send all the To: adresses */
	/* Either we're using the -t option, or we're using the arguments */
	if(minus_t) {
		struct string_node *node;

		if (list_empty(&rcpt_list))
			die("No recipients specified although -t option used");

		list_for_each(&rcpt_list, node, list) {
			p = rcpt_remap(node->string);
			outbytes += smtp_write(sock, "RCPT TO:<%s>", p);

			(void)alarm((unsigned)MEDWAIT);

			if(smtp_okay(sock, buf) == 0) {
				die("RCPT TO:<%s> (%s)", p, buf);
			}
		}
	}
	else {
		for(i = 1; (argv[i] != NULL); i++) {
			p = strtok(argv[i], ",");
			while(p) {
				/* RFC822 Address -> "foo@bar" */
				q = rcpt_remap(addr_parse(p));
				outbytes += smtp_write(sock, "RCPT TO:<%s>", q);

				(void)alarm((unsigned) MEDWAIT);

				if(smtp_okay(sock, buf) == 0) {
					die("RCPT TO:<%s> (%s)", q, buf);
				}

				p = strtok(NULL, ",");
			}
		}
	}

	/* Send DATA */
	outbytes += smtp_write(sock, "DATA");
	(void)alarm((unsigned) MEDWAIT);

	if(smtp_read(sock, buf) != 3) {
		/* Oops, we were expecting "354 send your data" */
		die("%s", buf);
	}

	outbytes += smtp_write(
		sock,
		"Received: by %s (sSMTP, from userid %d);\r\n\t%s",
		hostname,
		getuid(),
		get_arpadate_now_tz()
	);

	if(have_from == False) {
		outbytes += smtp_write(sock, "From: %s", from);
	}

	remote_addr = getenv("REMOTE_ADDR");
	if(remote_addr) {
		outbytes += smtp_write(sock, "X-Originating-IP: %s", remote_addr);
	}

	if(have_date == False) {
		outbytes += smtp_write(sock, "Date: %s", get_arpadate_now());
	}

#ifdef HASTO_OPTION
	if(have_to == False) {
		outbytes += smtp_write(sock, "To: postmaster");
	}
#endif

	list_for_each(&header_list, node, list) {
		outbytes += smtp_write(sock, "%s", node->string);
	}

	(void)alarm((unsigned) MEDWAIT);

	/* End of headers, start body */
	outbytes += smtp_write(sock, "");

	/*
	 * prevent blocking on pipes, we really shouldnt be using stdio
	 * functions like fgets in the first place
	 */
	if (fileno(input) == fileno(stdin))
		fcntl(fileno(input), F_SETFL,O_NONBLOCK);

	while(!feof(input)) {
		if (!fgets(buf, bufsize, input)) {
			/* if nothing was received, then no transmission
			 * over smtp should be done */
			sleep(1);
			/* don't hang forever when reading from input */
			if (++timeout >= MEDWAIT) {
				log_event(LOG_ERR, "killed: timeout on input "
						"while reading body -- message "
						"saved to dead.letter.");
				die("Timeout on input while reading body");
			}
			continue;
		}
		/* Trim off \n, double leading .'s */
		leadingdot = standardise(buf, &linestart);

		if (linestart || feof(input)) {
			linestart = True;
			outbytes += smtp_write(sock, "%s",
					leadingdot ? b : buf);
		} else {
			if (log_level > 0) {
				log_event(LOG_INFO, "Sent a very long line in "
						"chunks");
			}
			if (leadingdot) {
				outbytes += fd_puts(sock, b, strlen(b));
			} else {
				outbytes += fd_puts(sock, buf, strlen(buf));
			}
		}
		(void)alarm((unsigned) MEDWAIT);
	}
	if(!linestart) {
		smtp_write(sock, "");
	}
	/* End of body */

	outbytes += smtp_write(sock, ".");
	(void)alarm((unsigned) MAXWAIT);

	if(smtp_okay(sock, buf) == 0) {
		die("%s", buf);
	}

	/* Close connection */
	(void)signal(SIGALRM, SIG_IGN);

	outbytes += smtp_write(sock, "QUIT");
	(void)smtp_okay(sock, buf);
	(void)close(sock);

	log_event(LOG_INFO, "Sent mail for %s (%s) uid=%d username=%s "
			"outbytes=%d",
			from_strip(uad), buf, uid, pw->pw_name, outbytes);

	return(0);
}

/*
 * paq() - Write error message and exit
 */
static void
paq(char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	(void)vfprintf(stderr, format, ap);
	va_end(ap);

	exit(1);
}

/*
 * parse_options() -- Pull the options out of the command-line
 *	Process them (special-case calls to mailq, etc) and return the rest
 */
char **parse_options(int argc, char **argv)
{
	static char Version[] = VERSION;
	static char *new_argv[MAXARGS];
	int i, j, add, new_argc;

	new_argv[0] = argv[0];
	new_argc = 1;

	/* Set the globals */
	prog = basename(argv[0]);
	if (!prog) {
		perror("basename");
		paq("Cannot run basename on %s", argv[0]);
	}

	if(strcmp(prog, "mailq") == 0) {
		/* Someone wants to know the queue state... */
		paq("mailq: Mail queue is empty\n");
	}
	else if(strcmp(prog, "newaliases") == 0) {
		/* Someone wanted to rebuild aliases */
		paq("newaliases: Aliases are not used in sSMTP\n");
	}

	i = 1;
	while(i < argc) {
		if(argv[i][0] != '-') {
			new_argv[new_argc++] = argv[i++];
			continue;
		}
		j = 0;

		add = 1;
		while(argv[i][++j] != '\0') {
			switch(argv[i][j]) {
#ifdef INET6
			case '6':
				p_family = PF_INET6;
				continue;

			case '4':
				p_family = PF_INET;
				continue;
#endif

			case 'a':
				switch(argv[i][++j]) {
				case 'u':
					if((!argv[i][(j + 1)])
						&& argv[(i + 1)]) {
						auth_user = strdup(argv[i+1]);
						if(auth_user == (char *)NULL) {
							die("parse_options() --"
								" strdup() "
								"failed");
						}
						add++;
					}
					else {
						auth_user = strdup(argv[i]+j+1);
						if(auth_user == (char *)NULL) {
							die("parse_options() --"
								" strdup() "
								"failed");
						}
					}
					goto exit;

				case 'p':
					if((!argv[i][(j + 1)])
						&& argv[(i + 1)]) {
						auth_pass = strdup(argv[i+1]);
						if(auth_pass == (char *)NULL) {
							die("parse_options() --"
								" strdup() "
								"failed");
						}
						add++;
					}
					else {
						auth_pass = strdup(argv[i]+j+1);
						if(auth_pass == (char *)NULL) {
							die("parse_options() --"
								" strdup() "
								"failed");
						}
					}
					goto exit;

/*
#ifdef MD5AUTH
*/
				case 'm':
					if(!argv[i][j+1]) {
						auth_method = strdup(argv[i+1]);
						add++;
					}
					else {
						auth_method =
							strdup(argv[i]+j+1);
					}
				}
				goto exit;
/*
#endif
*/

			case 'b':
				switch(argv[i][++j]) {

				case 'a':	/* ARPANET mode */
					paq("-ba is not supported by sSMTP\n");
				case 'd':	/* Run as a daemon */
					paq("-bd is not supported by sSMTP\n");
				case 'i':	/* Initialise aliases */
					paq("%s: Aliases are not used in "
							"sSMTP\n", prog);
				case 'm':	/* Default addr processing */
					continue;

				case 'p':	/* Print mailqueue */
					paq("%s: Mail queue is empty\n", prog);
				case 's':	/* Read SMTP from stdin */
					paq("-bs is not supported by sSMTP\n");
				case 't':	/* Test mode */
					paq("-bt is meaningless to sSMTP\n");
				case 'v':	/* Verify names only */
					paq("-bv is meaningless to sSMTP\n");
				case 'z':	/* Create freeze file */
					paq("-bz is meaningless to sSMTP\n");
				}

			/* Configfile name */
			case 'C':
				if((!argv[i][(j + 1)]) && argv[(i + 1)]) {
					config_file = strdup(argv[(i + 1)]);
					if(config_file == (char *)NULL) {
						die("parse_options() --"
							" strdup() failed");
					}
					add++;
				}
				else {
					config_file = strdup(argv[i]+j+1);
					if(config_file == (char *)NULL) {
						die("parse_options() --"
							" strdup() failed");
					}
				}
				goto exit;

			/* Debug */
			case 'd':
				log_level = 1;
				/* Almost the same thing... */
				minus_v = True;

				continue;

			/* Insecure channel, don't trust userid */
			case 'E':
					continue;

			case 'R':
				/* Amount of the message to be returned */
				if(!argv[i][j+1]) {
					add++;
					goto exit;
				}
				else {
					/* Process queue for recipient */
					continue;
				}

			/* Fullname of sender */
			case 'F':
				if((!argv[i][(j + 1)]) && argv[(i + 1)]) {
					minus_F = strdup(argv[(i + 1)]);
					if(minus_F == (char *)NULL) {
						die("parse_options() --"
							" strdup() failed");
					}
					add++;
				}
				else {
					minus_F = strdup(argv[i]+j+1);
					if(minus_F == (char *)NULL) {
						die("parse_options() --"
							" strdup() failed");
					}
				}
				goto exit;

			/* Set from/sender address */
			case 'f':
			/* Obsolete -f flag */
			case 'r':
				if((!argv[i][(j + 1)]) && argv[(i + 1)]) {
					minus_f = strdup(argv[(i + 1)]);
					if(minus_f == (char *)NULL) {
						die("parse_options() --"
							" strdup() failed");
					}
					add++;
				}
				else {
					minus_f = strdup(argv[i]+j+1);
					if(minus_f == (char *)NULL) {
						die("parse_options() --"
							" strdup() failed");
					}
				}
				goto exit;

			/* Set hopcount */
			case 'h':
				continue;

			/* Ignore originator in adress list */
			case 'm':
				continue;

			/* Use specified message-id */
			case 'M':
				goto exit;

			/* DSN options */
			case 'N':
				add++;
				goto exit;

			/* No aliasing */
			case 'n':
				continue;

			case 'o':
				switch(argv[i][++j]) {

				/* Alternate aliases file */
				case 'A':
					goto exit;

				/* Delay connections */
				case 'c':
					continue;

				/* Run newaliases if required */
				case 'D':
					paq("%s: Aliases are not used in "
							"sSMTP\n", prog);

				/* Deliver now, in background or queue */
				/* This may warrant a diagnostic for b or q */
				case 'd':
						continue;

				/* Errors: mail, write or none */
				case 'e':
					j++;
					continue;

				/* Set tempfile mode */
				case 'F':
					goto exit;

				/* Save ``From ' lines */
				case 'f':
					continue;

				/* Set group id */
				case 'g':
					goto exit;

				/* Helpfile name */
				case 'H':
					continue;

				/* DATA ends at EOF, not \n.\n */
				case 'i':
					continue;

				/* Log level */
				case 'L':
					goto exit;

				/* Send to me if in the list */
				case 'm':
					continue;

				/* Old headers, spaces between adresses */
				case 'o':
					paq("-oo is not supported by sSMTP\n");

				/* Queue dir */
				case 'Q':
					goto exit;

				/* Read timeout */
				case 'r':
					goto exit;

				/* Always init the queue */
				case 's':
					continue;

				/* Stats file */
				case 'S':
					goto exit;

				/* Queue timeout */
				case 'T':
					goto exit;

				/* Set timezone */
				case 't':
					goto exit;

				/* Set uid */
				case 'u':
					goto exit;

				/* Set verbose flag */
				case 'v':
					minus_v = True;
					continue;
				}
				break;

			/* Process the queue [at time] */
			case 'q':
					paq("%s: Mail queue is empty\n", prog);

			/* Read message's To/Cc/Bcc lines */
			case 't':
				minus_t = True;
				continue;

			/* minus_v (ditto -ov) */
			case 'v':
				minus_v = True;
				break;

			/* Say version and quit */
			/* Similar as die, but no logging */
			case 'V':
				paq("sSMTP %s (Not sendmail at all)\n", Version);
			}
		}

exit:
		i += add;
	}
	new_argv[new_argc] = NULL;

	if(new_argc <= 1 && !minus_t) {
		paq("%s: No recipients supplied - mail will not be sent\n",
				prog);
	}

	if(new_argc > 1 && minus_t) {
		paq("%s: recipients with -t option not supported\n", prog);
	}

	return(&new_argv[0]);
}
