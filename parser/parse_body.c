/*
 * $Id$
 *
 * Copyright (C) 2008 iptelorg GmbH
 *
 * This file is part of ser, a free SIP server.
 *
 * ser is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version
 *
 * For a license to use the ser software under conditions
 * other than those described here, or to purchase support for this
 * software, please contact iptel.org by e-mail at the following addresses:
 *    info@iptel.org
 *
 * ser is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License 
 * along with this program; if not, write to the Free Software 
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * History:
 * --------
 *  2008-05-22  Initial version, get_body_part() is introduced (Miklos)
 */

/*! \file
 * \brief Parser :: Body handling
 *
 * \ingroup parser
 */


#include "../trim.h"
#include "parser_f.h"
#include "parse_content.h"
#include "parse_param.h"
#include "keys.h"
#include "parse_body.h"

/*! \brief returns the value of boundary parameter from the Contect-Type HF */
static inline int get_boundary_param(struct sip_msg *msg, str *boundary)
{
	str	s;
	char	*c;
	param_t	*p, *list;

#define is_boundary(c) \
	(((c)[0] == 'b' || (c)[0] == 'B') && \
	((c)[1] == 'o' || (c)[1] == 'O') && \
	((c)[2] == 'u' || (c)[2] == 'U') && \
	((c)[3] == 'n' || (c)[3] == 'N') && \
	((c)[4] == 'd' || (c)[4] == 'D') && \
	((c)[5] == 'a' || (c)[5] == 'A') && \
	((c)[6] == 'r' || (c)[6] == 'R') && \
	((c)[7] == 'y' || (c)[7] == 'Y'))

#define boundary_param_len (sizeof("boundary")-1)

	/* get the pointer to the beginning of the parameter list */
	s.s = msg->content_type->body.s;
	s.len = msg->content_type->body.len;
	c = find_not_quoted(&s, ';');
	if (!c)
		return -1;
	c++;
	s.len = s.len - (c - s.s);
	s.s = c;
	trim_leading(&s);

	if (s.len <= 0)
		return -1;

	/* parse the parameter list, and search for boundary */
	if (parse_params(&s, CLASS_ANY, NULL, &list)<0)
		return -1;

	boundary->s = NULL;
	for (p = list; p; p = p->next)
		if ((p->name.len == boundary_param_len) &&
			is_boundary(p->name.s)
		) {
			boundary->s = p->body.s;
			boundary->len = p->body.len;
			break;
		}
	free_params(list);
	if (!boundary->s || !boundary->len)
		return -1;

	DBG("boundary is \"%.*s\"\n",
		boundary->len, boundary->s);
	return 0;
}

/*! \brief search the next boundary in the buffer */
static inline char *search_boundary(char *buf, char *buf_end, str *boundary)
{
	char *c;

	c = buf;
	while (c + 2 /* -- */ + boundary->len < buf_end) {
		if ((*c == '-') && (*(c+1) == '-') &&
			(memcmp(c+2, boundary->s, boundary->len) == 0)
		)
			return c; /* boundary found */

		/* go to the next line */
		while ((c < buf_end) && (*c != '\n')) c++;
		c++;
	}
	return NULL;
}

/*! \brief extract the body of a part from a multipart SIP msg body */
inline static char *get_multipart_body(char *buf,
					char *buf_end,
					str *boundary,
					int *len)
{
	char *beg, *end;

	if (buf >= buf_end)
		goto error;

	beg = buf;
	while ((*beg != '\r') && (*beg != '\n')) {
		while ((beg < buf_end) && (*beg != '\n'))
			beg++;
		beg++;
		if (beg >= buf_end)
			goto error;
	}
	/* CRLF delimeter found, the body begins right after it */
	while ((beg < buf_end) && (*beg != '\n'))
		beg++;
	beg++;
	if (beg >= buf_end)
		goto error;

	if (!(end = search_boundary(beg, buf_end, boundary)))
		goto error;

	/* CRLF preceding the boundary belongs to the boundary
	and not to the body */
	if (*(end-1) == '\n') end--;
	if (*(end-1) == '\r') end--;

	if (end < beg)
		goto error;

	*len = end-beg;
	return beg;
error:
	ERR("failed to extract the body from the multipart mime type\n");
	return NULL;
}


/*! \brief macros from parse_hname2.c */
#define READ(val) \
(*(val + 0) + (*(val + 1) << 8) + (*(val + 2) << 16) + (*(val + 3) << 24))

#define LOWER_DWORD(d) ((d) | 0x20202020)

/*! \brief Returns the pointer within the msg body to the given type/subtype,
 * and sets the length of the body part.
 * The result can be the whole msg body, or a part of a multipart body.
 */
char *get_body_part(	struct sip_msg *msg,
			unsigned short type, unsigned short subtype,
			int *len)
{
	int	mime;
	unsigned int	umime;
	char	*c, *c2, *buf_end;
	str	boundary;

#define content_type_len \
	(sizeof("Content-Type") - 1)

	if ((mime = parse_content_type_hdr(msg)) <= 0)
		return NULL;

	if (mime == ((type<<16)|subtype)) {
		/* content-type is type/subtype */
		c = get_body(msg);
		if (c)
			*len = msg->buf+msg->len - c;
		return c;

	} else if ((mime>>16) == TYPE_MULTIPART) {
		/* type is multipart/something, search for type/subtype part */

		if (get_boundary_param(msg, &boundary)) {
			ERR("failed to get boundary parameter\n");
			return NULL;
		}
		if (!(c = get_body(msg)))
			return NULL;
		buf_end = msg->buf+msg->len;

		/* check all the body parts delimated by the boundary value,
		and search for the Content-Type HF with the given 
		type/subtype */
next_part:
		while ((c = search_boundary(c, buf_end, &boundary))) {
			/* skip boundary */
			c += 2 + boundary.len;

			if ((c+2 > buf_end) ||
				((*c == '-') && (*(c+1) == '-'))
			)
				/* end boundary, no more body part
				will follow */
				return NULL;

			/* go to the next line */
			while ((c < buf_end) && (*c != '\n')) c++;
			c++;
			if (c >= buf_end)
				return NULL;

			/* try to find the content-type header */
			while ((*c != '\r') && (*c != '\n')) {
				if (c + content_type_len >= buf_end)
					return NULL;

				if ((LOWER_DWORD(READ(c)) == _cont_) &&
					(LOWER_DWORD(READ(c+4)) == _ent__) &&
					(LOWER_DWORD(READ(c+8)) == _type_)
				) {
					/* Content-Type HF found */
					c += content_type_len;
					while ((c < buf_end) &&
						((*c == ' ') || (*c == '\t'))
					)
						c++;

					if (c + 1 /* : */ >= buf_end)
						return NULL;

					if (*c != ':')
						/* not realy a Content-Type HF */
						goto next_hf;
					c++;

					/* search the end of the header body,
					decode_mime_type() needs it */
					c2 = c;
					while (((c2 < buf_end) && (*c2 != '\n')) ||
						((c2+1 < buf_end) && (*c2 == '\n') &&
							((*(c2+1) == ' ') || (*(c2+1) == '\t')))
					)
						c2++;

					if (c2 >= buf_end)
						return NULL;
					if (*(c2-1) == '\r') c2--;

					if (!decode_mime_type(c, c2 , &umime)) {
						ERR("failed to decode the mime type\n");
						return NULL;
					}

					/* c2 points to the CRLF at the end of the line,
					move the pointer to the beginning of the next line */
					c = c2;
					if ((c < buf_end) && (*c == '\r')) c++;
					if ((c < buf_end) && (*c == '\n')) c++;

					if (umime != ((type<<16)|subtype)) {
						/* this is not the part we are looking for */
						goto next_part;
					}

					/* the requested type/subtype is found! */
					return get_multipart_body(c,
							buf_end,
							&boundary,
							len);
				}
next_hf:
				/* go to the next line */
				while ((c < buf_end) && (*c != '\n')) c++;
				c++;
			}
			/* CRLF delimeter reached,
			no Content-Type HF was found */
		}
	}
	return NULL;
}