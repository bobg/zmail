/*
 * Copyright (c) 1994 Z-Code Software Corp., an NCD company.
 */

#include <general/general.h>
#include <errno.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/file.h>
#include "popper.h"
#include <general/dynstr.h>
#include <general/dpipe.h>
#include <general/dputil.h>
#include <mstore/mime-api.h>
#include <mstore/message.h>
#include <bfuncs.h>
#include <general/strcase.h>
#include <zctime.h>

static const char pop_dropinfo_rcsid[] =
    "$Id: pop_dropinfo.c,v 1.32 1996/04/23 22:26:00 spencer Exp $";

#undef MIN
#define MIN(a,b) (((a) < (b)) ? (a) : (b))

/* from mstore/mime-api.c */
DECLARE_EXCEPTION(mime_err_Header);
DEFINE_EXCEPTION(spool_format_err, "spool file format error");

/* from zync_extras.c, for constructing "From " from From: lines where nec. */
extern char *parse_address_and_name P((char *, char *, char *));

static void
call_mime_pair_destroy(ptr, ignored)
    VPTR ptr, ignored;
{
    mime_pair_destroy(ptr);
}

/* Find any character in s1 that's in s2; return pointer to that char in s1.
   stolen from shell/zstrings.c */
char *
any(s1, s2)
     register const char *s1, *s2;
{
    register const char *p;

    if (!s1 || !*s1 || !s2 || !*s2)
	return NULL;
    for( ; *s1; s1++) {
	for(p = s2; *p; p++)
	    if (*p == *s1)
		return (char *) s1;
    }
    return NULL;
}

/* this strcpy returns number of bytes copied (stolen from shell/zstrings.c) */
static int
Strcpy(dst, src)
     register char *dst;
     register const char *src;
{
    register char *d = dst;

    if (!dst || !src)
	return 0;
    while (*dst++ = *src++)
	;
    return dst - d - 1;
}

/*
 * stolen from msgs/addrs.c
 *
 * Convert RFC822 or mixed addresses to RFC976 `!' form,
 *  copying the new address to d.  The source address is
 *  translated according to RFC822 rules.
 * Return a pointer to the end (nul terminus) of d.
 */
static char *
bang_form (d, s)
char *d, *s;
{
    char *r, *t, *ab = NULL;

    *d = '\0';
    /* If nothing to do, quit now */
    if (!s || !*s) {
	return d;
    }
    /* Avoid any angle braces */
    if (*s == '<') {
	if (ab = index(s + 1, '>'))
	    s++, *ab = '\0';
	else
	    return NULL;
    }
    /*
     * Look backwards for the first `@'; this gives us the
     * primary domain of the RFC822 address
     */
    if (*s == '@') {
	/* An RFC-822 "@domain1,@domain2:" routing */
	if (t = any(++s, ",:")) {
	    char c = *t;
	    *t = '\0';
	    d += Strcpy(d, s);
	    *d++ = '!';
	    *t++ = c;
	    r = bang_form(d, t);
	} else
	    r = NULL;
    } else if ((t = rindex(s, '@')) && t != s) {
	/* Copy the RFC822 domain as the UUCP head */
	d += Strcpy(d, t + 1);
	*d++ = '!';
	*t = '\0';
	r = bang_form(d, s);
	*t = '@';
    } else if (t = index(s, '!')) {
	/* A normal UUCP path */
	*t = '\0';
	d += Strcpy(d, s);
	*t++ = *d++ = '!';
	r = bang_form(d, t);
    } else if (t = rindex(s, '%')) {
	/* An imbedded `%' -- treat as low-priority `@' */
	*t = '@';
	r = bang_form(d, s);
	*t = '%';
    } else
	r = d + Strcpy(d, s);  /* No `@', `!', or `%' */
    if (ab)
	*ab = '>';
    return r;
}

/*
 * eat silly ">From " lines generated by ancient UUCP delivery programs
 * - dp is the dpipe that we are reading from.
 * - fp, if not NULL, is a FILE * to write out any ">From " lines that we
 *   eat. this is needed in pop_updt when we write the drop file back to
 *   the spool file, since we're just ignoring the extra ">From " lines
 *   and not actually consolidating them
 * - from_line is a dynstr containing the "From " line we already read;
 *   any ">From " lines will be folded into this dynstr.
 */
static off_t
eat_uucp_from_line(dpp, from_line)
    struct dpipe *dpp;
    struct dynstr *from_line;
{
    off_t bytesread = 0, thislen;
    struct dynstr d;
    const char *newline;
    char user[256], path[MAXMSGLINELEN+1], *s;

    user[0] = path[0] = '\0';
    dynstr_Init(&d);
    while (!dpipe_Eof(dpp)) {
	dynstr_Set(&d, 0);
	newline = mime_Readline(dpp, &d);
	thislen = dynstr_Length(&d);
	if (newline)
	    thislen += strlen(newline);
	if (strncmp(dynstr_Str(&d), ">From ", 6)) {
	    /* burp */ /* (Jesus, Kim!) */
	    dynstr_Append(&d, newline);	
	    dpipe_Unget(dpp, dynstr_GiveUpStr(&d), thislen);
	    break;
	}
	s = dynstr_Str(&d)+6;
	(void)sscanf(s, "%s", user);
	while (s = index(s+1, 'r')) {
	    if (!strncmp(s, "remote from ", 12)) {
		char *p2 = path+strlen(path);
		/* skipspaces(12); */
		for (s += 12; *s == ' ' || *s == '\t'; ++s)
		    ;
		/* add the new machine to path */
		(void) sscanf(s, "%s", p2);
		(void) strcat(p2, "!");
		break;
	    }
	}
	bytesread += thislen;
    }
    if (user[0])
	bang_form(path+strlen(path), user);
    if (path[0]) {
	char *origfrom = dynstr_GiveUpStr(from_line);
	sscanf(origfrom+5, "%s", user);
	dynstr_Init(from_line);
	dynstr_Set(from_line, "From ");
	dynstr_Append(from_line, path);
	dynstr_Append(from_line, origfrom+5+strlen(user));
	free(origfrom);
    }
    return bytesread;
}

/*
 * <spencer> Fri Mar 22 18:00:06 1996
 * here are the specs for the find_next_{start,end}_boundary functions
 *
 * find_next_start_boundary takes a POP *, a dpipe *, and a dynstr *.
 * these are the pop structure, the pipe we're reading from, and an initialized
 * dynstr in which to store the "From " line, if any.  it should consume all
 * input up to and including the start boundary of the next message, including
 * the "From " line if it exists, including any silly UUCP ">From " lines, and
 * return the total number of bytes it consumed.  if there is no "From " line
 * then the dynstr * should be set to a null string.
 *
 * find_next_end_boundary takes a POP *, a dpipe *, an off_t, and an off_t *.
 * these are the pop structure, the pipe we're reading from, the minimum
 * content length we should read before checking for a separator, and a handle
 * to an off_t in which to return the length of the end boundary.  it should
 * consume all input up to and including the end boundary of the current
 * message, and return the total number of bytes consumed.  the length of the
 * boundary is important because pop_dropinfo uses the number of bytes
 * consumed to compute the size of the message body; since this function
 * returns the size of the message body plus the size of the trailing boundary,
 * we have to pass that info back.  don't forget to include the length of
 * the newline following the boundary.
 *
 * in the case of v7 folders, both the start boundary and end boundary are
 * effectively null strings (the "From " line is considered special and not
 * a part of the boundary per se).
 *
 * i think the concept of start and end boundaries is more useful than that of
 * a separator, since there are ^A's before and after the message in MMDF.
 */

static off_t
v7_find_next_start_boundary(p, dp, fromline)
    POP *p;
    struct dpipe *dp;
    struct dynstr *fromline;
{
    off_t bytesread = 0;
    int garbage = 0;
    const char *newline;

    while (!dpipe_Eof(dp)) {
	off_t thislen;

	dynstr_Set(fromline, 0);
	newline = mime_Readline(dp, fromline);
	thislen = dynstr_Length(fromline);
	if (newline)
	    thislen += strlen(newline);
	bytesread += thislen;
	if (0 == strncmp("From ", dynstr_Str(fromline), 5)) {
	    /* v7_find_next_end_boundary already made sure it was valid */
	    if (garbage) {
		/* we know this is the first message, otherwise we would
		 * have hit EOF in v7_find_next_end_boundary */
		if (p->debug & DEBUG_ERRORS)
		    pop_log(p, POP_PRIORITY,
			    "%ld bytes of garbage before first message",
			    bytesread - thislen);
		RAISE(spool_format_err, "v7_find_next_start_boundary");
	    }
	    bytesread += eat_uucp_from_line(dp, fromline);
	    break;
	} else {
	    char *p2;
	    for (p2 = dynstr_Str(fromline); *p2; p2++) {
		if (*p2 != ' ' && *p2 != '\t') {
		    garbage = 1;
		    break;
		}
	    }
	}
    }
    if (garbage) {
	if (p->debug & DEBUG_ERRORS)
	    pop_log(p, POP_PRIORITY,
		    "%ld bytes of garbage after last message",
		    bytesread);
	RAISE(spool_format_err, "v7_find_next_start_boundary");
    }
    return bytesread;
}

extern int is_from_line P((char *));

static off_t
v7_find_next_end_boundary(p, dp, content_length, separator_length)
    POP *p;
    struct dpipe *dp;
    off_t content_length, *separator_length;
{
    off_t bytesread = 0;
    struct dynstr d;
    const char *newline;

    dynstr_Init(&d);
    while (content_length > 0) {
	if (dpipe_Eof(dp)) {
	    if (p->debug & DEBUG_ERRORS)
		pop_log(p, POP_PRIORITY,
			"premature EOF (expected %d more bytes)",
			content_length);
	    content_length = 0;
	    break;
	}
	dynstr_Set(&d, 0);
	newline = mime_Readline(dp, &d);
	bytesread += dynstr_Length(&d);
	content_length -= dynstr_Length(&d);
	if (newline) {
	    bytesread += strlen(newline);
	    content_length -= strlen(newline);
	}
    }
    /* Now we can look for the "From " line */
    while (!dpipe_Eof(dp)) {
	off_t thislen;

	dynstr_Set(&d, 0);
	newline = mime_Readline(dp, &d);
	thislen = dynstr_Length(&d);
	if (newline)
	    thislen += strlen(newline);
	if ((0 == strncmp("From ", dynstr_Str(&d), 5))
	    && is_from_line(dynstr_Str(&d))) {
	    dynstr_Append(&d, newline);	
	    dpipe_Unget(dp, dynstr_GiveUpStr(&d), thislen);
	    break;
	}
	bytesread += thislen;
    }
    if (dpipe_Eof(dp))
	dynstr_Destroy(&d);
    *separator_length = 0;
    return bytesread;
}

static off_t
mmdf_find_next_start_boundary(p, dp, fromline)
    POP *p;
    struct dpipe *dp;
    struct dynstr *fromline;
{
    off_t bytesread = 0;
    int garbage = 0;
    const char *newline;

    while (!dpipe_Eof(dp)) {
	off_t thislen;

	dynstr_Set(fromline, 0);
	newline = mime_Readline(dp, fromline);
	thislen = dynstr_Length(fromline);
	if (newline)
	    thislen += strlen(newline);
	bytesread += thislen;
	if (0 == strcmp(dynstr_Str(&p->msg_separator), dynstr_Str(fromline))) {
	    if (garbage) {
		if (p->debug & DEBUG_ERRORS)
		    pop_log(p, POP_PRIORITY,
			    "%ld bytes of garbage before start of next message",
			    bytesread-thislen);
		RAISE(spool_format_err,
		      "mmdf_find_next_start_boundary");
	    }
	    if (!dpipe_Eof(dp)) {
		dynstr_Set(fromline, 0);
		newline = mime_Readline(dp, fromline);
		thislen = dynstr_Length(fromline);
		if (newline)
		    thislen += strlen(newline);
		if (0 == strncmp("From ", dynstr_Str(fromline), 5)) {
		    bytesread += thislen;
		    bytesread += eat_uucp_from_line(dp, fromline);
		} else {
		    dynstr_Append(fromline, newline);
		    dpipe_Unget(dp, dynstr_GiveUpStr(fromline), thislen);
		    dynstr_Set(fromline, 0);
		}
	    }
	    break;
	} else {
	    char *p2;
	    for (p2 = dynstr_Str(fromline); *p2; p2++) {
		if (*p2 != ' ' && *p2 != '\t') {
		    garbage = 1;
		    break;
		}
	    }
	}
    }
    if (garbage) {
	if (p->debug & DEBUG_ERRORS)
	    pop_log(p, POP_PRIORITY,
		    "%ld bytes of garbage after last message",
		    bytesread);
	RAISE(spool_format_err, "mmdf_find_next_start_boundary");
    }
    return bytesread;
}

static off_t
mmdf_find_next_end_boundary(p, dp, content_length, separator_length)
    POP *p;
    struct dpipe *dp;
    off_t content_length, *separator_length;
{
    off_t bytesread = 0;
    struct dynstr d;
    const char *newline;

    dynstr_Init(&d);
    TRY {
	while (!dpipe_Eof(dp)) {
	    off_t thislen;

	    dynstr_Set(&d, 0);
	    newline = mime_Readline(dp, &d);
	    thislen = dynstr_Length(&d);
	    if (newline)
		thislen += strlen(newline);
	    bytesread += thislen;
	    if (0 == strcmp(dynstr_Str(&p->msg_separator), dynstr_Str(&d))) {
		*separator_length = thislen;
		break;
	    }
	}
    } FINALLY {
	dynstr_Destroy(&d);
    } ENDTRY;
    return bytesread;
}

extern char *parse_date P((char *, time_t));

/* the dimensions of this function are 1x4x9 */
void
pop_dropinfo(p)
    POP *p;
{
    struct glist info_glist, hlist;
    struct msg_info *minfo = 0;
    const char *newline;
    struct dynstr d;
    struct dpipe dp;
    off_t offset = 0;
    off_t (*find_next_start_boundary) NP((POP *, struct dpipe *,
					  struct dynstr *));
    off_t (*find_next_end_boundary) NP((POP *, struct dpipe *,
					off_t, off_t *));

    rewind(p->drop);

    dpipe_Init(&dp, 0, 0, dputil_FILEtoDpipe, p->drop, 0);
    glist_Init(&hlist, (sizeof (struct mime_pair)), 8);

    dynstr_Init(&d);
    TRY {
	glist_Init(&info_glist, (sizeof (struct msg_info)), 8);

	TRY {
	    struct dynstr fromline;

	    p->msgs_deleted = 0;
	    p->orig_last_msg = 0;
	    p->last_msg = 0;
	    p->bytes_deleted = 0;

	    if (p->spool_format & MMDF_SEPARATORS) {
		find_next_start_boundary = mmdf_find_next_start_boundary;
		find_next_end_boundary = mmdf_find_next_end_boundary;
	    } else {
		find_next_start_boundary = v7_find_next_start_boundary;
		find_next_end_boundary = v7_find_next_end_boundary;
	    }

	    /* Skip to first "From " line */
	    dynstr_Init(&fromline);
	    do {
		offset += find_next_start_boundary(p, &dp, &fromline);
		if (dpipe_Eof(&dp)) {
#ifdef OLD_WAY
		    /* got EOF either before the boundary or immediately
		     * following it */
		    if (!offset)
			break;
		    if (p->debug & DEBUG_ERRORS)
			pop_log(p, POP_PRIORITY,
				"Reached EOF before message boundary");
		    RAISE(mime_err_Header, "pop_dropinfo");
#else
		    break;
#endif /* OLD_WAY */
		}
		/* We've just read the "From " line; it started
		 * at offset_was
		 */
		if (Verbose)
		    pop_log(p, POP_DEBUG,
			    "Parsing msg %d at offset %ld...",
			    glist_Length(&info_glist) + 1, offset);
		glist_Add(&info_glist, 0);
		minfo = (struct msg_info *) glist_Last(&info_glist);
		dynstr_Init(&minfo->from_line);
		if (dynstr_Length(&fromline)) {
		    minfo->had_from_line = 1;
		    dynstr_Set(&minfo->from_line, dynstr_Str(&fromline));
		}
		minfo->have_key_hash = 0;
		minfo->have_header_hash = 0;
		minfo->have_date = 0;
		minfo->summary = 0;
		minfo->unique_id = 0;
		minfo->status = mmsg_status_NEW | mmsg_status_UNREAD;
		minfo->number = glist_Length(&info_glist);
		minfo->header_offset = offset;
		minfo->header_adjust = 0;

		glist_Map(&hlist, call_mime_pair_destroy, 0);
		glist_Truncate(&hlist, 0);
		if (less_than_mime_Headers(&dp, &hlist, 0)) {
		    char *paren;
		    off_t content_length = 0, separator_length;
		    struct mime_pair *mp;
		    int i;

		    glist_FOREACH(&hlist, struct mime_pair, mp, i) {
			offset += dynstr_Length(&(mp->name));
			minfo->header_adjust += mp->offset_adjust;
			offset += mp->offset_adjust + 1; /* +1 for colon */
			offset += dynstr_Length(&(mp->value));
			if ((p->spool_format & CONTENT_LENGTH)
			    && !ci_strcmp("content-length",
					  dynstr_Str(&(mp->name)))) {
			    if (content_length <= 0)
				content_length =
				    atol(dynstr_Str(&(mp->value)));
			} else if (!dynstr_Length(&fromline)
				   && !ci_strcmp("from",
						 dynstr_Str(&mp->name))) {
			    char addr[256], *p;
			    time_t stupid;

			    parse_address_and_name(dynstr_Str(&mp->value),
						   addr, NULL);
			    dynstr_Set(&minfo->from_line, addr);
			    dynstr_AppendChar(&minfo->from_line, ' ');
			    time(&stupid);
			    strcpy(addr, ctime(&stupid));
			    p = addr+strlen(addr);
			    while ('\n' == *--p)
				*p = '\0';
			    dynstr_Append(&minfo->from_line, addr);
			} else if (!ci_strcmp("status",
					      dynstr_Str(&(mp->name)))) {
			    parse_status(dynstr_Str(&(mp->value)),
					 &(minfo->status));
			    if (!(minfo->status &
				  (mmsg_status_NEW | mmsg_status_UNREAD)))
				p->last_msg = minfo->number;
			} else if (!ci_strcmp("date",
					      dynstr_Str(&(mp->name)))) {
			    char *parsed;

			    mime_Unfold(&(mp->value), 1);
			    parsed = parse_date(dynstr_Str(&(mp->value)),
						0);
			    if (parsed) {
				sscanf(parsed, "%ld", &(minfo->date));
				minfo->have_date = 1;
			    }
			} else if (!ci_strcmp("x-key-digest",
					      dynstr_Str(&(mp->name)))
				   && (!(paren =
					 index(dynstr_Str(&(mp->value)),
					       '('))
				       || (atoi(paren + 1) ==
					   glist_Length(&hlist)))) {
			    string_to_mailhash(&(minfo->key_hash),
					       dynstr_Str(&(mp->value)));
			    minfo->have_key_hash = 1;
			}
		    }

		    /* Read past the newline following the headers */
		    dynstr_Set(&d, 0);
		    newline = mime_Readline(&dp, &d);
		    offset += dynstr_Length(&d);
		    if (newline)
			offset += strlen(newline);
		    minfo->body_offset = offset;
		    minfo->header_length = offset - minfo->header_offset;
		    offset += find_next_end_boundary(p, &dp,
						     content_length,
						     &separator_length);
		    minfo->body_length =
			offset - minfo->body_offset - separator_length;
		    if (Verbose)
			pop_log(p, POP_DEBUG,
				"...%ld header bytes, %ld body bytes",
				minfo->header_length, minfo->body_length);
		} else {
		    if (p->debug & DEBUG_ERRORS)
			pop_log(p, POP_PRIORITY,
				"mime_Headers returned 0 at offset %ld",
				offset);
		    RAISE(mime_err_Header, "pop_dropinfo");
		}
	    } while (!dpipe_Eof(&dp));
	    p->drop_size = offset;
	    dynstr_Destroy(&fromline);
	} EXCEPT(spool_format_err) {
	    static char informative[256];
	    int msgnum = glist_Length(&info_glist);

	    if (msgnum)
		sprintf(informative, "after message %d near offset %ld in %s",
			msgnum, offset, p->tmpdropname);
	    else
		sprintf(informative, "before first message in %s",
			p->tmpdropname);
	    glist_Destroy(&info_glist);
	    except_SetExceptionValue(informative);
	    PROPAGATE();
	} EXCEPT(mime_err_Header) {
	    static char informative[256];

	    sprintf(informative,
		    "Syntax error in headers of message %d near offset %ld in %s",
		    glist_Length(&info_glist), offset, p->tmpdropname);
	    glist_Destroy(&info_glist);
	    except_SetExceptionValue(informative);
	    PROPAGATE();
	} EXCEPT(ANY) {
	    glist_Destroy(&info_glist);
	    PROPAGATE();
	} ENDTRY;
	bcopy(&info_glist, &(p->minfo), (sizeof (struct glist)));
    } FINALLY {
	dynstr_Destroy(&d);
	dpipe_Destroy(&dp);
	glist_CleanDestroy(&hlist,
			   (void (*) NP((VPTR))) mime_pair_destroy);
    } ENDTRY;
}