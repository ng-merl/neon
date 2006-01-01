/* 
   URI manipulation routines.
   Copyright (C) 1999-2006, Joe Orton <joe@manyfish.co.uk>

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public
   License as published by the Free Software Foundation; either
   version 2 of the License, or (at your option) any later version.
   
   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public
   License along with this library; if not, write to the Free
   Software Foundation, Inc., 59 Temple Place - Suite 330, Boston,
   MA 02111-1307, USA

*/

#include "config.h"

#ifdef HAVE_STRING_H
#include <string.h>
#endif
#ifdef HAVE_STRINGS_H
#include <strings.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif

#include <stdio.h>

#include <ctype.h>

#include "ne_string.h" /* for ne_buffer */
#include "ne_alloc.h"
#include "ne_uri.h"

/* URI ABNF from RFC 3986: */

#define PS (0x0001) /* "+" */
#define PC (0x0002) /* "%" */
#define DS (0x0004) /* "-" */
#define DT (0x0008) /* "." */
#define US (0x0010) /* "_" */
#define TD (0x0020) /* "~" */
#define FS (0x0040) /* "/" */
#define CL (0x0080) /* ":" */
#define AT (0x0100) /* "@" */
#define QU (0x0200) /* "?" */

#define DG (0x0400) /* DIGIT */
#define AL (0x0800) /* ALPHA */

#define GD (0x1000) /* gen-delims    = "#" / "[" / "]" 
                     * ... except ":", "/", "@", and "?" */

#define SD (0x2000) /* sub-delims    = "!" / "$" / "&" / "'" / "(" / ")"
                     *               / "*" / "+" / "," / ";" / "=" 
                     * ... except "+" which is PS */

#define OT (0x4000) /* others */

#define URI_ALPHA (AL)
#define URI_DIGIT (DG)

/* unreserved = ALPHA / DIGIT / "-" / "." / "_" / "~" */
#define URI_UNRESERVED (AL | DG | DS | DT | US | TD)
/* scheme = ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ) */
#define URI_SCHEME (AL | DG | PS | DS | DT)
/* real sub-delims definition, including "+" */
#define URI_SUBDELIM (PS | SD)
/* real gen-delims definition, including ":", "/", "@" and "?" */
#define URI_GENDELIM (GD | CL | FS | AT | QU)
/* userinfo = *( unreserved / pct-encoded / sub-delims / ":" ) */
#define URI_USERINFO (URI_UNRESERVED | PC | URI_SUBDELIM | CL)
/* pchar = unreserved / pct-encoded / sub-delims / ":" / "@" */
#define URI_PCHAR (URI_UNRESERVED | PC | URI_SUBDELIM | CL | AT)
/* invented: segchar = pchar / "/" */
#define URI_SEGCHAR (URI_PCHAR | FS)
/* query = fragment = *( pchar / "/" / "?" ) */
#define URI_QUERY (URI_PCHAR | FS | QU)

/* any characters which should be path-escaped: */
#define URI_ESCAPE ((URI_GENDELIM & ~(FS)) | URI_SUBDELIM | OT)

static const unsigned int uri_chars[256] = {
/* 0xXX    x0      x2      x4      x6      x8      xA      xC      xE     */
/*   0x */ OT, OT, OT, OT, OT, OT, OT, OT, OT, OT, OT, OT, OT, OT, OT, OT,
/*   1x */ OT, OT, OT, OT, OT, OT, OT, OT, OT, OT, OT, OT, OT, OT, OT, OT,
/*   2x */ OT, SD, OT, GD, SD, PC, SD, SD, SD, SD, SD, PS, SD, DS, DT, FS,
/*   3x */ DG, DG, DG, DG, DG, DG, DG, DG, DG, DG, CL, SD, OT, SD, OT, QU,
/*   4x */ AT, AL, AL, AL, AL, AL, AL, AL, AL, AL, AL, AL, AL, AL, AL, AL,
/*   5x */ AL, AL, AL, AL, AL, AL, AL, AL, AL, AL, AL, GD, OT, GD, OT, US,
/*   6x */ OT, AL, AL, AL, AL, AL, AL, AL, AL, AL, AL, AL, AL, AL, AL, AL,
/*   7x */ AL, AL, AL, AL, AL, AL, AL, AL, AL, AL, AL, OT, OT, OT, TD, OT,
/*   8x */ OT, OT, OT, OT, OT, OT, OT, OT, OT, OT, OT, OT, OT, OT, OT, OT, 
/*   9x */ OT, OT, OT, OT, OT, OT, OT, OT, OT, OT, OT, OT, OT, OT, OT, OT, 
/*   Ax */ OT, OT, OT, OT, OT, OT, OT, OT, OT, OT, OT, OT, OT, OT, OT, OT, 
/*   Bx */ OT, OT, OT, OT, OT, OT, OT, OT, OT, OT, OT, OT, OT, OT, OT, OT, 
/*   Cx */ OT, OT, OT, OT, OT, OT, OT, OT, OT, OT, OT, OT, OT, OT, OT, OT, 
/*   Dx */ OT, OT, OT, OT, OT, OT, OT, OT, OT, OT, OT, OT, OT, OT, OT, OT, 
/*   Ex */ OT, OT, OT, OT, OT, OT, OT, OT, OT, OT, OT, OT, OT, OT, OT, OT, 
/*   Fx */ OT, OT, OT, OT, OT, OT, OT, OT, OT, OT, OT, OT, OT, OT, OT, OT
};

#define uri_lookup(ch) (uri_chars[(unsigned)ch])

char *ne_path_parent(const char *uri) 
{
    size_t len = strlen(uri);
    const char *pnt = uri + len - 1;
    /* skip trailing slash (parent of "/foo/" is "/") */
    if (pnt >= uri && *pnt == '/')
	pnt--;
    /* find previous slash */
    while (pnt > uri && *pnt != '/')
	pnt--;
    if (pnt < uri || (pnt == uri && *pnt != '/'))
	return NULL;
    return ne_strndup(uri, pnt - uri + 1);
}

int ne_path_has_trailing_slash(const char *uri) 
{
    size_t len = strlen(uri);
    return ((len > 0) &&
	    (uri[len-1] == '/'));
}

unsigned int ne_uri_defaultport(const char *scheme)
{
    /* RFC2616/3.2.3 says use case-insensitive comparisons here. */
    if (strcasecmp(scheme, "http") == 0)
	return 80;
    else if (strcasecmp(scheme, "https") == 0)
	return 443;
    else
	return 0;
}

int ne_uri_parse(const char *uri, ne_uri *parsed)
{
    const char *p, *s;

    memset(parsed, 0, sizeof *parsed);

    if (uri[0] == '\0') {
	return -1;
    }

    p = s = uri;

    if (uri_lookup(*p) & URI_ALPHA) {
        while (uri_lookup(*p) & URI_SCHEME)
            p++;
        
        if (*p == ':') {
            parsed->scheme = ne_strndup(uri, p - s);
            s = p + 1;
        }
    }

    if (s[0] == '/' && s[1] == '/') {
        const char *pa;

        /* hier-part = "//" authority path-abempty 
         * authority = [ userinfo "@" ] host [ ":" port ] */

        s = pa = s + 2; /* => s = authority */

        while (*pa != '/' && *pa != '\0')
            pa++;
        /* => pa = path-abempty */
        
        p = s;
        while (p < pa && uri_lookup(*p) & URI_USERINFO)
            p++;

        if (*p == '@') {
            parsed->userinfo = ne_strndup(s, p - s);
            s = p + 1;
        }
        /* => s = host */

        if (s[0] == '[') {
            p = s + 1;

            while (*p != ']' && p < pa)
                p++;

            if (p == pa || (p + 1 != pa && p[1] != ':')) {
                /* Ill-formed IP-literal. */
                return -1;
            }

            p++; /* => p = colon */
        } else {
            /* Find the colon. */
            p = pa;
            while (*p != ':' && p > s)
                p--;
        }

        if (p == s) {
            p = pa;
            /* No colon; => p = path-abempty */
        } else if (p + 1 != pa) {
            /* => p = colon */
            parsed->port = atoi(p + 1);
        }
        parsed->host = ne_strndup(s, p - s);
        
        s = pa;        

        if (*s == '\0') {
            s = "/"; /* FIXME: scheme-specific. */
        }
    } 
    /* else, the path begins at s */

    p = s;

    while (uri_lookup(*p) & URI_SEGCHAR)
        p++;

    parsed->path = ne_strndup(s, p - s);

    if (*p != '\0') {
        s = p++;

        while (uri_lookup(*p) & URI_QUERY)
            p++;

        if (*s == '?') {
            parsed->query = ne_strndup(s + 1, p - s - 1);
            
            if (*p != '\0') {
                s = p++;

                while (uri_lookup(*p) & URI_QUERY)
                    p++;
            }
        }
        /* p must now point to the end of the input string */

        if (*s == '#') {
            parsed->fragment = ne_strndup(s + 1, p - s - 1);
        }
        else if (*p || *s != '?') {
            return -1;
        }
    }
    
    return 0;
}

void ne_uri_free(ne_uri *u)
{
    if (u->host) ne_free(u->host);
    if (u->path) ne_free(u->path);
    if (u->scheme) ne_free(u->scheme);
    if (u->userinfo) ne_free(u->userinfo);
    memset(u, 0, sizeof *u);
}

char *ne_path_unescape(const char *uri) 
{
    const char *pnt;
    char *ret, *retpos, buf[5] = { "0x00\0" };
    retpos = ret = ne_malloc(strlen(uri) + 1);
    for (pnt = uri; *pnt != '\0'; pnt++) {
	if (*pnt == '%') {
	    if (!isxdigit((unsigned char) pnt[1]) || 
		!isxdigit((unsigned char) pnt[2])) {
		/* Invalid URI */
                ne_free(ret);
		return NULL;
	    }
	    buf[2] = *++pnt; buf[3] = *++pnt; /* bit faster than memcpy */
	    *retpos++ = (char)strtol(buf, NULL, 16);
	} else {
	    *retpos++ = *pnt;
	}
    }
    *retpos = '\0';
    return ret;
}

/* CH must be an unsigned char; evaluates to 1 if CH should be
 * percent-encoded. */
#define path_escape_ch(ch) (uri_lookup(ch) & URI_ESCAPE)

char *ne_path_escape(const char *path) 
{
    const unsigned char *pnt;
    char *ret, *p;
    size_t count = 0;

    for (pnt = (const unsigned char *)path; *pnt != '\0'; pnt++) {
        count += path_escape_ch(*pnt);
    }

    if (count == 0) {
	return ne_strdup(path);
    }

    p = ret = ne_malloc(strlen(path) + 2 * count + 1);
    for (pnt = (const unsigned char *)path; *pnt != '\0'; pnt++) {
	if (path_escape_ch(*pnt)) {
	    /* Escape it - %<hex><hex> */
	    sprintf(p, "%%%02x", (unsigned char) *pnt);
	    p += 3;
	} else {
	    *p++ = *pnt;
	}
    }
    *p = '\0';
    return ret;
}

#undef path_escape_ch

#define CASECMP(field) do { \
n = strcasecmp(u1->field, u2->field); if (n) return n; } while(0)

#define CMP(field) do { \
n = strcmp(u1->field, u2->field); if (n) return n; } while(0)

/* As specified by RFC 2616, section 3.2.3. */
int ne_uri_cmp(const ne_uri *u1, const ne_uri *u2)
{
    int n;
    
    if (u1->path[0] == '\0' && strcmp(u2->path, "/") == 0)
	return 0;
    if (u2->path[0] == '\0' && strcmp(u1->path, "/") == 0)
	return 0;

    CMP(path);
    CASECMP(host);
    CASECMP(scheme);
    if (u1->port > u2->port)
	return 1;
    else if (u1->port < u2->port)
	return -1;
    return 0;
}

#undef CMP
#undef CASECMP

#ifndef WIN32
#undef min
#define min(a,b) ((a)<(b)?(a):(b))
#endif

/* TODO: implement properly */
int ne_path_compare(const char *a, const char *b) 
{
    int ret = strcasecmp(a, b);
    if (ret) {
	/* This logic says: "If the lengths of the two URIs differ by
	 * exactly one, and the LONGER of the two URIs has a trailing
	 * slash and the SHORTER one DOESN'T, then..." */
	int traila = ne_path_has_trailing_slash(a),
	    trailb = ne_path_has_trailing_slash(b),
	    lena = strlen(a), lenb = strlen(b);
	if (traila != trailb && abs(lena - lenb) == 1 &&
	    ((traila && lena > lenb) || (trailb && lenb > lena))) {
	    /* Compare them, ignoring the trailing slash on the longer
	     * URI */
	    if (strncasecmp(a, b, min(lena, lenb)) == 0)
		ret = 0;
	}
    }
    return ret;
}

char *ne_uri_unparse(const ne_uri *uri)
{
    ne_buffer *buf = ne_buffer_create();

    ne_buffer_concat(buf, uri->scheme, "://", 
                     uri->userinfo ? uri->userinfo : "",
                     uri->userinfo ? "@" : "",
                     uri->host, NULL);

    if (uri->port > 0 && ne_uri_defaultport(uri->scheme) != uri->port) {
	char str[20];
	ne_snprintf(str, 20, ":%d", uri->port);
	ne_buffer_zappend(buf, str);
    }

    ne_buffer_zappend(buf, uri->path);

    if (uri->query) {
        ne_buffer_concat(buf, "?", uri->query, NULL);
    }
    
    if (uri->fragment) {
        ne_buffer_concat(buf, "#", uri->fragment, NULL);
    }

    return ne_buffer_finish(buf);
}

/* Give it a path segment, it returns non-zero if child is 
 * a child of parent. */
int ne_path_childof(const char *parent, const char *child) 
{
    char *root = ne_strdup(child);
    int ret;
    if (strlen(parent) >= strlen(child)) {
	ret = 0;
    } else {
	/* root is the first of child, equal to length of parent */
	root[strlen(parent)] = '\0';
	ret = (ne_path_compare(parent, root) == 0);
    }
    ne_free(root);
    return ret;
}
