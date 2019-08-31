/* 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * For more ,please contact QQ/wechat:4108863 mail:4108863@qq.com
 */

#define _GNU_SOURCE

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <ctype.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <stdio.h>



#include "httpx.h"
#include "ssl_array.h"
#include "rules.h"
#include "wwwfiles.h"
#include "ssl_utils.h"
#include "ssl_json.h"

#undef _GNU_SOURCE








ngx_str_t chkvalue;


/*
** Structures related to the decode parser
*/
typedef struct  {
  char	*prefix;
  void   (*pars)(char *, http_waf_msg *);
} http_decode_parser_t;



/*
 * Line can have end with: '\n', '\r\n' or '\n\r'
 */
const char *find_line_end(const char *data, const char *dataend, const char **eol)
{
    const char *lineend;

    lineend = memchr(data, '\n', dataend - data);
    if (lineend == NULL) {
        lineend = dataend;
        *eol = dataend;
    }
    else {
        if (lineend != data) {
            if (*(lineend - 1) == '\r') {
                *eol = lineend - 1;
            }
            else {
                *eol = lineend;
                if (lineend != (dataend - 1) && *(lineend + 1) == '\r') {
                    lineend++;
                }
            }
        }
        else {
            *eol = lineend;
            if (lineend != (dataend - 1) && *(lineend + 1) == '\r') {
                lineend++;
            }
        }
        lineend++;
    }
    
    return lineend;
}




int find_line_end_unquoted(char *line, int len, int *next_offset)
{
    char *token_a, *token_b, *linep;
    char *cr, *lf;
    int linelen;
    //bool quote;
    
    linelen = 0;
   // quote = FALSE;
    *next_offset = len;
    token_a = memchr(line, '"', len);
    cr = memchr(line, '\r', len);
    lf = memchr(line, '\n', len);
    if (token_a != NULL) {
        if ((cr == NULL || token_a < cr) && (lf == NULL || token_a < lf)) {
            token_a++;
            token_b = memchr(token_a, '"', len - (token_a - line));
            if (token_b != NULL) {
                linep = token_a;
                while (linep != token_b && *linep != '\r' && *linep != '\n')
                    linep++;
                if (linep != token_b) {
                    linelen = token_b - token_a;
                    *next_offset = token_b - line;
                }
            }
        }
    }
    else if (cr != NULL) {
        linelen = cr - line; /* without \r, \n, or ...*/
        if (cr != line + len - 1) {
            if (cr[1] == '\n')
                cr++;
            *next_offset = cr - line + 1;
        }
        else {
            *next_offset = len;
        }
    } 
    else if (lf != NULL) {
        linelen = lf - line; /* without \r, \n, or ...*/
        if (lf != line + len - 1) {
            if (lf[1] == '\n')
                lf++;
            *next_offset = lf - line + 1;
        }
        else {
            *next_offset = len;
        }
    }
    
    
    return linelen;
}



int find_chr(const char *line, int len, char c)
{
    int i;
    
    i = 0;
    
    while (i != len && line[i] != c)
        i++;
    if (i == len)
        return -1;

    return i;
}



int get_token_len(const char *linep, const char *lineend, const char **next_token)
{
    const char *tokenp;
    int token_len;

    tokenp = linep;

    while (linep != lineend && *linep != ' ' && *linep != '\r' && *linep != '\n')
        linep++;
    token_len = linep - tokenp;

    while (linep != lineend && *linep == ' ')
        linep++;

    *next_token = linep;

    return token_len;
}


static char* http_uri(const char *line, int len)
{
    const char *next_token;
    const char *lineend;
    int tokenlen;
    char *uri;
    
    /* \r\n necesary for bug in client POST */
    if (len > 1 && strncmp(line, "\r\n", 2) == 0) {
        len -= 2;
        line += 2;
    }
    lineend = line + len;

    /* The first token is the method. */
    tokenlen = get_token_len(line, lineend, &next_token);
    if (tokenlen == 0 || line[tokenlen] != ' ') {
        return NULL;
    }
    line = next_token;

    /* The next token is the URI. */
    tokenlen = get_token_len(line, lineend, &next_token);
    if (tokenlen == 0 || line[tokenlen] != ' ')
        return NULL;

    //uri = DMemMalloc(tokenlen+1);
    if (uri != NULL) {
        memcpy(uri, line, tokenlen);
        uri[tokenlen] = '\0';
    }

    return uri;
}



static inline char *http_uri_ext(const char *uri)
{
    int i;
    char *ext;

    /* extension file name */
    if (uri == NULL)
        return NULL;
    
    ext = strrchr(uri, '.');
    if (ext != NULL) {
        ext++;
        if (strlen(ext) > 4)
            ext = NULL;
        else {
            i = 0;
            while (ext[i] != '\0') {
                if (!((ext[i] >= 'a' && ext[i] <= 'z') || (ext[i] >= 'A' && ext[i] <= 'Z'))) {
                    ext = NULL;
                    break;
                }
                i++;
            }
        }
    }
    
    return ext;
}


static http_ver http_req_version(const char *line, int len)
{
    const char *next_token;
    const char *lineend;
    int tokenlen;

    lineend = line + len;

    /* The first token is the method. */
    tokenlen = get_token_len(line, lineend, &next_token);
    if (tokenlen == 0 || line[tokenlen] != ' ') {
        return HTTP_VER_NONE;
    }
    line = next_token;

    /* The next token is the URI. */
    tokenlen = get_token_len(line, lineend, &next_token);
    if (tokenlen == 0 || line[tokenlen] != ' ')
        return HTTP_VER_NONE;
    line = next_token;

    /* Everything to the end of the line is the version. */
    tokenlen = lineend - line;
    if (tokenlen == 0)
        return HTTP_VER_NONE;
    
    if (strncmp(line, "HTTP/1.0", 8) == 0)
        return HTTP_VER_1_0;
    
    if (strncmp(line, "HTTP/1.1", 8) == 0)
        return HTTP_VER_1_1;

    return HTTP_VER_NONE;
}



static http_mthd http_req_method(const char *data, int linelen)
{
    const char *ptr;
    int	index = 0;
    //char *unkn;

    /*
     * From RFC 2774 - An HTTP Extension Framework
     *
     * Support the command prefix that identifies the presence of
     * a "mandatory" header.
     */
    if (linelen >= 2) {
        if (strncmp(data, "M-", 2) == 0 || strncmp(data, "\r\n", 2) == 0) { /* \r\n necesary for bug in client POST */
            data += 2;
            linelen -= 2;
        }
    }
    
    /*
     * From draft-cohen-gena-client-01.txt, available from the uPnP forum:
     *	NOTIFY, SUBSCRIBE, UNSUBSCRIBE
     *
     * From draft-ietf-dasl-protocol-00.txt, a now vanished Microsoft draft:
     *	SEARCH
     */
    ptr = (const char *)data;
    /* Look for the space following the Method */
    //while (index != linelen) {
    while(*ptr != '\0'){
        if (*ptr == ' ')
            break;
        else {
            ptr++;
            index++;
        }
    }

    /* Check the methods that have same length */
    switch (data[0]) {
    case 'G':
        if (strncmp(data, "GET", index) == 0) {
            return HTTP_MT_GET;
        }
        break;
        
    case 'P':
        if (strncmp(data, "POST", index) == 0) {
            return HTTP_MT_POST;
        }
        else if (strncmp(data, "PUT", index) == 0) {
            return HTTP_MT_PUT;
        }
        else if (strncmp(data, "POLL", index) == 0) {
            return HTTP_MT_POLL;
        }
        else if (strncmp(data, "PROPFIND", index) == 0) {
            return HTTP_MT_PROPFIND;
        }
        else if (strncmp(data, "PROPPATCH", index) == 0) {
            return HTTP_MT_PROPPATCH;
        }
        else if (strncmp(data, "PATCH", index) == 0) {
            return HTTP_MT_PATCH;
        }
        break;
        
    case 'B':
        if (strncmp(data, "BCOPY", index) == 0) {
            return HTTP_MT_BCOPY;
        }
        else if (strncmp(data, "BMOVE", index) == 0) {
            return HTTP_MT_BMOVE;
        }
        else if (strncmp(data, "BDELETE", index) == 0) {
            return HTTP_MT_BDELETE;
        }
        else  if (strncmp(data, "BPROPFIND", index) == 0) {
            return HTTP_MT_BPROPFIND;
        }
        else if (strncmp(data, "BPROPPATCH", index) == 0) {
            return HTTP_MT_BPROPPATCH;
        }
        else if (strncmp(data, "BASELINE-CONTROL", index) == 0) {  /* RFC 3253 12.6 */
            return HTTP_MT_BASELINE_CONTROL;
        }
        break;
        
    case 'C':
        if (strncmp(data, "COPY", index) == 0) {
            return HTTP_MT_COPY;
        }
        else if (strncmp(data, "CONNECT", index) == 0) {
            return HTTP_MT_CONNECT;
        }
        else if (strncmp(data, "CHECKIN", index) == 0) {  /* RFC 3253 4.4, 9.4 */
            return HTTP_MT_CHECKIN;
        }
        else if (strncmp(data, "CHECKOUT", index) == 0) { /* RFC 3253 4.3, 9.3 */
            return HTTP_MT_CHECKOUT;
        }
        /*
        else if (strncmp(data, "CCM_POST", index) == 0) {
            return HTTP_MT_CCM_POST;
        }
        */
        break;
        
    case 'D':
        if (strncmp(data, "DELETE", index) == 0) {
            return HTTP_MT_DELETE;
        }
        break;
        
    case 'H':
        if (strncmp(data, "HEAD", index) == 0) {
            return HTTP_MT_HEAD;
        }
        break;
        
#if 0
    case 'I':
        if (strncmp(data, "ICY", index) == 0) {
            return HTTP_MT_ICY;
        }
        break;
#endif
        
    case 'L':
        if (strncmp(data, "LOCK", index) == 0) {
            return HTTP_MT_LOCK;
        }
        else if (strncmp(data, "LINK", index) == 0) {
            return HTTP_MT_LINK;
        }
        else if (strncmp(data, "LABEL", index) == 0) {  /* RFC 3253 8.2 */
            return HTTP_MT_LABEL;
        }
        break;
        
    case 'M':
        if (strncmp(data, "MOVE", index) == 0) {
            return HTTP_MT_MOVE;
        }
        else if (strncmp(data, "MKCOL", index) == 0) {
            return HTTP_MT_MKCOL;
        }
        else if (strncmp(data, "MERGE", index) == 0) {  /* RFC 3253 11.2 */
            return HTTP_MT_MERGE;
        }
        else if (strncmp(data, "MKACTIVITY", index) == 0) {  /* RFC 3253 13.5 */
            return HTTP_MT_MKACTIVITY;
        }
        else if (strncmp(data, "MKWORKSPACE", index) == 0) {  /* RFC 3253 6.3 */
            return HTTP_MT_MKWORKSPACE;
        }
        break;
        
    case 'N':
        if (strncmp(data, "NOTIFY", index) == 0) {
            return HTTP_MT_NOTIFY;
        }
        break;
        
    case 'O':
        if (strncmp(data, "OPTIONS", index) == 0) {
            return HTTP_MT_OPTIONS;
        }
        break;
        
    case 'S':
        if (strncmp(data, "SEARCH", index) == 0) {
            return HTTP_MT_SEARCH;
        }
        else if (strncmp(data, "SUBSCRIBE", index) == 0) {
            return HTTP_MT_SUBSCRIBE;
        }
        break;
        
    case 'T':
        if (strncmp(data, "TRACE", index) == 0) {
            return HTTP_MT_TRACE;
        }
        break;
        
    case 'U':
        if (strncmp(data, "UNLOCK", index) == 0) {
            return HTTP_MT_UNLOCK;
        }
        else if (strncmp(data, "UNLINK", index) == 0) {
            return HTTP_MT_UNLINK;
        }
        else if (strncmp(data, "UPDATE", index) == 0) {  /* RFC 3253 7.1 */
            return HTTP_MT_UPDATE;
        }
        else if (strncmp(data, "UNCHECKOUT", index) == 0) {  /* RFC 3253 4.5 */
            return HTTP_MT_UNCHECKOUT;
        }
        else if (strncmp(data, "UNSUBSCRIBE", index) == 0) {
            return HTTP_MT_UNSUBSCRIBE;
        }
        break;
        
    case 'V':
        if (strncmp(data, "VERSION-CONTROL", index) == 0) {  /* RFC 3253 3.5 */
            return HTTP_MT_VERSION_CONTROL;
        }
        break;
        
    case 'R':
        if (strncmp(data, "REPORT", index) == 0) {  /* RFC 3253 3.6 */
            return HTTP_MT_REPORT;
        }
        /*
        else if (strncmp(data, "RPC_CONNECT", index) == 0) {
            return HTTP_MT_RPC_CONNECT;
        }
        */
        break;
    }

	/*
    if (index > 0 && !test) {
        unkn = DMemMalloc(index+1);
        memcpy(unkn, data, index);
        unkn[index] = '\0';
        LogPrintf(LV_WARNING, "Http method (%s) don't managed.", unkn);
        DMemFree(unkn);
    }*/

    return HTTP_MT_NONE;
}



static char* http_head_end(const char *header, unsigned long len)
{
    const char *lf, *nxtlf, *end;
    const char *buf_end;
   
    end = NULL;
    buf_end = header + len;
    lf =  memchr(header, '\n', len);
    if (lf == NULL)
        return NULL;
    lf++; /* next charater */
    nxtlf = memchr(lf, '\n', buf_end - lf);
    while (nxtlf != NULL) {
        if (nxtlf-lf < 2) {
            end = nxtlf;
            break;
        }
        nxtlf++;
        lf = nxtlf;
        nxtlf = memchr(nxtlf, '\n', buf_end - nxtlf);
    }

    return (char *)end;
}

unsigned long
ngx_atoof(char *line, size_t n)
{
    unsigned long  value;

    if (n <= 0) {
        return 0;
    }

    for (value = 0; n--; line++) {
        if (*line < '0' || *line > '9') {
            return 0;
        }

        if (value > 4294836225) {
            return 0;
        }

        value = value * 10 + (*line - '0');
    }

    return value;
}

static char	*
strnchr(const char *s, int c, int len)
{
  int	cpt;
  for (cpt = 0; cpt < len && s[cpt]; cpt++)
    if (s[cpt] == c) 
      return ((char *) s+cpt);
  return (NULL);
}





				


/*
** does : parse body data, a.k.a POST/PUT datas. identifies content-type,
**	  and, if appropriate, boundary. then parse the stuff if multipart/for..
**	  or rely on spliturl if application/x-w..
** this function sucks ! I don't parse bigger-than-body-size posts that 
**	   are partially stored in files, TODO ;)
*/

/*
** Parse content-disposition line.
*/
int
nx_content_disposition_parse(unsigned char *str, unsigned char *line_end,
			     unsigned char **fvarn_start, unsigned char **fvarn_end,
			     unsigned char **ffilen_start, unsigned char **ffilen_end) 
{
  
  unsigned char *varn_start = NULL, *varn_end = NULL;
  unsigned char *filen_start = NULL, *filen_end = NULL;
  /* we have two cases :
  ** ---- file upload
  ** Content-Disposition: form-data; name="somename"; filename="NetworkManager.conf"\r\n
  ** Content-Type: application/octet-stream\r\n\r\n
  ** <DATA>
  ** ---- normal post var
  ** Content-Disposition: form-data; name="lastname"\r\n\r\n
  ** <DATA>
  */
  
  
  while (str < line_end) {
    /* rfc allow spaces and tabs inbetween */
    while (str < line_end && *str && (*str == ' ' || *str == '\t'))
      str++;
    if (str < line_end && *str && *str == ';')
      str++;
    while (str < line_end && *str && (*str == ' ' || *str == '\t'))
      str++;
    
    if (str >= line_end || !*str) 
      break;
    
    if (!ngx_strncmp(str, "name=\"", 6)) {
      /* we already successfully parsed a name, reject that. */
      if (varn_end || varn_start)
	return (NGX_ERROR);
      varn_end = varn_start = str + 6;
      do {
	varn_end = (unsigned char *) ngx_strchr(varn_end, '"');
	if (!varn_end || (varn_end && *(varn_end - 1) != '\\'))
	  break;
	varn_end++;
      } while (varn_end && varn_end < line_end);
      if (!varn_end   || !*varn_end)
	return (NGX_ERROR);
      str = varn_end;
      if (str < line_end+1)
	str++;
      else
	return (NGX_ERROR);
      *fvarn_start = varn_start;
      *fvarn_end = varn_end;
    }
    else if (!ngx_strncmp(str, "filename=\"", 10)) {
      /* we already successfully parsed a filename, reject that. */
      if (filen_end || filen_start)
	return (NGX_ERROR);
      filen_end = filen_start = str + 10;
      do {
	filen_end = (unsigned char *) ngx_strchr(filen_end, '"');
	if (!filen_end) break;
	if (filen_end && *(filen_end - 1) != '\\')
	  break;
	filen_end++;
      } while (filen_end && filen_end < line_end);
      if (!filen_end)
	return (NGX_ERROR);
      str = filen_end;
      if (str < line_end+1)
	str++;
      else
	return (NGX_ERROR);
      *ffilen_end = filen_end;
      *ffilen_start = filen_start;
    }
    else if (str == line_end -1)
      break;
    else {
      /* gargabe is present ?
      NX_DEBUG(_debug_post_heavy,     NGX_LOG_DEBUG_HTTP, r->connection->log, 0, 
		    "extra data in content-disposition ? end:%p, str:%p, diff=%d", line_end, str, line_end-str);*/

      return (NGX_ERROR);
    }
  }
  /* tssk tssk */
  if (filen_end > line_end || varn_end > line_end)
    return (NGX_ERROR);
  return (NGX_OK);
}



static void split_args(char *args,http_waf_msg *req)
{

	ngx_str_t	name, val;
    char		*eq, *ev, *orig,*str;
	int		len, full_len;
	int nullbytes = 0;

	str = args;
	orig = str;
    full_len = strlen(orig);
	while (str < (orig+full_len) && *str) 
	{
	    if (*str == '&') {			
	      str++;
	      continue;
	    }
		eq = strchr(str, '=');
    	ev = strchr(str, '&');
		
		if ((!eq && !ev) /*?foobar */ ||	(eq && ev && eq > ev)) /*?foobar&bla=test*/ 
		{		
		      if (!ev)
			  	ev = str+strlen(str);
		      /* len is now [name] */
		      len = ev - str;
		      val.data = (unsigned char *) str;
		      val.len = ev - str;
		      name.data = (unsigned char *) NULL;
		      name.len = 0;
    	}
		 /* ?&&val | ?var&& | ?val& | ?&val | ?val&var */
	    else if (!eq && ev) 
		{ 
	     
		      if (ev > str) /* ?var& | ?var&val */ 
			  {
				val.data = (unsigned char *) str;
				val.len = ev - str;
				name.data = (unsigned char *) NULL;
				name.len = 0;
				len = ev - str;
			  }
			  else /* ?& | ?&&val */ 
			  {
				val.data = name.data = NULL;
				val.len = name.len = 0;
				len = 1;
		      }
	    }
		else /* should be normal like ?var=bar& ..*/ 
		{
		      if (!ev) /* ?bar=lol */
			  	ev = str+strlen(str);
		      /* len is now [name]=[content] */
		      len = ev - str;
		      eq = strnchr(str, '=', len);
		      if (!eq) /*malformed url, possible attack*/
			  {	
				return ;
		      }
		      eq++;
		      val.data = (unsigned char *) eq;
		      val.len = ev - eq;
		      name.data = (unsigned char *) str;
		      name.len = eq - str - 1;
    	}

		if (name.len) 
		{
      		nullbytes = naxsi_unescape(&name);
      		name.data[name.len] = '\0';
      		//printf("%d		name=%.32s						",name.len,name.data);
		}
		if (val.len) 
		{
      		nullbytes = naxsi_unescape(&val);
      		val.data[val.len] = '\0';
			chk_all_rules(&val,ARGS,req);
      		//printf("%d		value=%.32s \n",val.len,val.data);
		}
      	//printf("\n");

		str += len; 
		str++;

		
	}
	
	
}

static void multipart_parse(u_char *src,http_waf_msg * req)
{
    ngx_str_t			final_var, final_data;
	u_char				 *boundary,*varn_start, *varn_end;
	u_char				*filen_start, *filen_end;
	u_char				*end, *line_end;
	u_int 			    len,boundary_len,varn_len, varc_len, idx, nullbytes;

	idx = 0;
	len = req->req_body_size;
	boundary_len = req->boundary_len;
	boundary = req->boundary;

	if(boundary_len < 3 || boundary == NULL)
		return;
	
	while (idx < len) 
	{
		 /* if we've reached the last boundary '--' + boundary + '--' + '\r\n'$END */
   		 /* Authorize requests that don't have the leading \r\n */
	    if (idx+boundary_len+6 == len || idx+boundary_len+4 == len) 
		{
	      if (ngx_strncmp(src+idx, "--", 2) ||
			  ngx_strncmp(src+idx+2, boundary, boundary_len) ||
			  ngx_strncmp(src+idx+boundary_len+2, "--", 2)) 
		  {
			/* bad closing boundary ?*/
			
			req->err_state = HTTP_MSG_ERROR;
			return ;
	      } 
		  else
				break;
	    }

		 /* --boundary\r\n : New var */
	    if ((len - idx < 4 + boundary_len) || src[idx] != '-' || src[idx+1] != '-' || 
			/* and if it's really followed by a boundary */
			ngx_strncmp(src+idx+2, boundary, boundary_len) || 
			/* and if it's not the last boundary of the buffer */
			idx+boundary_len + 2 + 2  >= len ||  
			/* and if it's followed by \r\n */
			src[idx+boundary_len+2] != '\r' || src[idx+boundary_len+3] != '\n') 
		{
		    /* bad boundary */
			//printf("!!!!!!! boundary over-------------%x %x---------%d    %s\n",src[idx+boundary_len+2],src[idx+boundary_len+3],len - idx,src+idx+2);
		    req->err_state = HTTP_MSG_ERROR;
		    return ;
	    }

		idx += boundary_len + 4;
	    /* we have two cases :
	    ** ---- file upload
	    ** Content-Disposition: form-data; name="somename"; filename="NetworkManager.conf"\r\n
	    ** Content-Type: application/octet-stream\r\n\r\n
	    ** <DATA>
	    ** ---- normal post var
	    ** Content-Disposition: form-data; name="lastname"\r\n\r\n
	    ** <DATA>
	    */
	    /* 31 = echo -n "content-disposition: form-data;" | wc -c */
    	if (ngx_strncasecmp(src+idx, (u_char *) "content-disposition: form-data;", 31)) 
		{		      
			  //printf("!!!!!!!error boundary----------------Unknown content-type: %s", src+idx);
		      req->err_state = HTTP_MSG_ERROR;		     
		      return ;
    	}

		idx += 31;
	    line_end = (u_char *) ngx_strchr(src+idx, '\n');
	    if (!line_end)
		{
	       req->err_state = HTTP_MSG_ERROR;
	       return ;
	    }

		/* Parse content-disposition, extract name / filename */
	    varn_start = varn_end = filen_start = filen_end = NULL;
	    if (nx_content_disposition_parse(src+idx, line_end, &varn_start, &varn_end,
					     &filen_start, &filen_end) != NGX_OK) {
	      req->err_state = HTTP_MSG_ERROR;
	      return ;
	    }
	    /* var name is mandatory */
	    if (!varn_start || !varn_end || varn_end <= varn_start) 
		{
		  //printf("!!!!!!!error boundary--------------varn_start-----------------------\n");	
	      req->msg_state = HTTP_MSG_ERROR;
	      return ;
	    }
		
		varn_len = varn_end - varn_start;
		
		/* If there is a filename, it is followed by a "content-type" line, skip it */
	    if (filen_start && filen_end) 
		{
	      line_end = (u_char *) ngx_strchr(line_end+1, '\n');
	      if (!line_end)
		  {		
			  req->err_state = HTTP_MSG_ERROR;		     
		      return ;		
	      }
    	}

		/* 
	    ** now idx point to the end of the 
	    ** content-disposition: form-data; filename="" name=""
	    */
	    idx += (u_char *)line_end - (src+idx) + 1;
	    if (src[idx] != '\r' || src[idx+1] != '\n') 
		{
		     req->err_state = HTTP_MSG_ERROR;
		     return ;
	    }


		
		if (filen_start) 
		{
			final_var.data = (unsigned char *)varn_start;
			final_var.len = varn_len;			
			final_data.data = (unsigned char *)filen_start;
			final_data.len = filen_end - filen_start;
							
			final_var.data[final_var.len] = '\0';
			final_data.data[final_data.len] = '\0'; 	
			//printf("filen_start -----------%d	%s	 %d  %s\n",final_var.len,final_var.data,final_data.len,final_data.data);	
			chk_all_rules(&final_data,FILE_EXT,req);
						
		}
		

		idx += 2;
	    /* seek the end of the data */
	    end = NULL;
	    while (idx < len) 
		{
		    end = (u_char *) ngx_strstr(src+idx, "\r\n--");
		    /* file data can contain \x0 */
		    while (!end) 
			{
				idx += strlen((const char *)src+idx);
				if (idx < len - 2) 
				{
				  idx++;
				  end = (u_char *) ngx_strstr(src+idx, "\r\n--");
				}
				else
				  break;
		    }
	       if (!end) 
		   {
				printf("POST data : malformed content-disposition line,perhaps file transfer \n");
				return ;
	      }
	      if (!ngx_strncmp(end+4, boundary, boundary_len))
				break;
	      else 
		  {
		  		//printf("error POST data : end +4=%s\n",end+4);
				idx += ((u_char *) end - (src+idx)) + 1;
				end = NULL;
	      }
	    }

		if (!end) 
		{
		     printf("POST data : malformed line\n");
		     return ;
    	}

			
		if (filen_start) 		
				idx += (u_char *) end - (src+idx);
		

		else if (varn_start)
		{
				varc_len = (u_char *) end - (src+idx);
				final_var.data = (unsigned char *)varn_start;
				final_var.len = varn_len;
				final_data.data = src+idx;
				final_data.len = varc_len;
					
				final_var.data[final_var.len] = '\0';
				final_data.data[final_data.len] = '\0';					
				//printf("varn_start -----------%d	 %s   %d  %s\n",final_var.len,final_var.data,final_data.len,final_data.data);	
				chk_all_rules(&final_data,ARGS,req);
				idx += (u_char *) end - (src+idx);
			
		}
		else
		{
			printf("(multipart) : --------------------do nothing\n");
		}
	
				
		//if (!ngx_strncmp(end, "\r\n--", 2)) 		
      	idx += 2;
				
		
	}
	
	return;

}


static void decode_uri(char *uri,http_waf_msg * req)
{
	char *p,*args = NULL;
	int uri_len = strlen(uri);
	int i;
		
	
	/*  GET /aaa/bbb.html?args=22&aaa# HTTP/1.1 */
	p = strrchr(uri,' ');
	if(!p || uri[0] != '/')
	{
		req->err_state  = HTTP_MSG_ERROR;
		return;
	}
	*p = '\0';
	
	for(i = 0;i < uri_len;i++)
	{
		
		if(uri[i] == '?')
		{
			if(args == NULL)
			{
				uri[i] = '\0';
				args = uri + i +1;
			}
		}
	}	

	(void)find_www_file(uri,req);
	


	/*bbb.html*/
	p  = strrchr(uri,'/');
	*p = '\0';
	
	p++;
	req->req_file = p;
	req->req_dir = uri;

	
		
	//printf("URI=%s/%s\n",req->req_dir,req->req_file);
	chkvalue.data = p;
	chkvalue.len  = strlen(p);
	chk_all_rules(&chkvalue,URL,req);
	
	
	
	/* args=22&aaa#*/
	if(args != NULL)
		split_args(args,req);

}
static void decode_cookie(char *cookie,http_waf_msg * req)
{
	//printf("Cookie=%s\n",cookie);
	chkvalue.data = cookie;
	chkvalue.len  = strlen(cookie);
	chk_all_rules(&chkvalue,COOKIE,req);

}

static void decode_content_len(char *p,http_waf_msg * req)
{
	req->content_len  = ngx_atoof(p,strlen(p));	
}

static void decode_content_type(char *p,http_waf_msg * req)
{
	char *h;
	int len;

	if(strcasestr(p,"application/json"))	
		req->content_type = HTTP_CONTENT_JSON;
		
	
	else if(strcasestr(p,"application/csp-report"))
		req->content_type = HTTP_CONTENT_JSON;
	
	
	else if(strcasestr(p,"multipart/form-data"))	
	{
		h = strstr(p,"boundary=");
		if(!h)
			return;
		h += 9;

		/* RFC 1867/1341 says 70 char max, 
     	I arbitrarily set min to 3 (yes) */
		len = strlen(h);
		if(len<3 || len>70)
		{
			req->err_state = HTTP_MSG_ERROR;
			return;

		}
		req->boundary_len = len;
		req->boundary = h;
		req->content_type = HTTP_CONTENT_MULTIPART;
		//printf("boundary_len=%d  %s\n",req->boundary_len,req->boundary);
	}
	else
		req->content_type = 0;
}


static http_decode_parser_t httpx_parser[] = {
  {"GET ", decode_uri},
  {"POST ", decode_uri},
  {"Cookie: ", decode_cookie},
  {"Content-Length: ", decode_content_len},
  {"Content-Type: ", decode_content_type},
  
 
  {NULL, NULL}
};


static int check_null_header(http_waf_msg *req)
{
	char *p = req->buf;
	int i;
	int nullbytes = 0;


	for(i = 0;i < req->req_hdr_size - 1;i++)
	{
		if(p[i] == 0)
		{
			p[i] = '0';
			req->err_state = HTTP_MSG_ERROR;
			nullbytes ++;
			
		}
		p[i] = tolower(p[i]);
	}


	return nullbytes;
	
}



static void decode_header(http_waf_msg *req)
{
	char *p;	
	int i,len;

	if(check_null_header(req) > 0 )
		printf("error null headers\n");
	
	for (p = strtok(req->buf, "\r\n"); p; p = strtok(NULL, "\r\n"))
  	{
  		
		for (i = 0; httpx_parser[i].pars; i++) 
		{	
			len =  strlen(httpx_parser[i].prefix);
		    if (!strncasecmp(p, httpx_parser[i].prefix, len))	
				httpx_parser[i].pars(p+len,req);	
			//chk_all_rules(&final_data,HEADERS);
			if(req->rule_id > 100) //matched attack
				return;
				
      	}
  	}


}
static void process_body(http_waf_msg *req)
{
	if(req->only_once == 1)
		return;
	//printf("req->msg_state=%d body_size=%d content_len=%d body=%s\n",req->msg_state,req->req_body_size,req->content_len,req->req_body);	
	if(req->content_type == HTTP_CONTENT_MULTIPART)
		multipart_parse(req->req_body,req);
	else if(req->content_type == HTTP_CONTENT_JSON)
		ngx_http_dummy_json_parse(req->req_body,req->req_body_size,req);
	else
		split_args(req->req_body,req);
	
}


static void init_http_status(http_waf_msg *req)
{
	req->content_len   = 0;
	req->content_type  = 0;
	req->boundary_len  = 0;
	req->only_once     = 0;	
	req->log_msg       = NULL;

}

static void process_header(http_waf_msg *req)
{
		
	char *end;
	char *buf = req->buf;
	int   len = req->pos;
	
	req->mtd = http_req_method(buf, 0);		
	/*maybe body data such as file transfer*/
	if(req->mtd != HTTP_MT_GET && req->mtd != HTTP_MT_POST) 
	{
		//printf("NOT get or post--------------req->mtd:%d %d %s",req->mtd,len,buf);
		//req->msg_state = HTTP_MSG_DONE;
		return;
	}	
	
	/* decode http get */
	if(req->msg_state != HTTP_MSG_BODY)
	{
		end = http_head_end(buf,len);
		if(end != NULL)
		{	
			//printf("%s\n",req->buf);
			*end = '\0';
			end++;
			len = end - buf;
			
			req->req_hdr_size  = len;
			req->req_body      = end;
			req->req_body_size = req->pos - len;		

			
			init_http_status(req);
			decode_header(req);
				
						
			
			if(req->req_body_size >= req->content_len)
				req->msg_state = HTTP_MSG_DONE;
			else
				req->msg_state = HTTP_MSG_BODY;

		
			if(req->msg_state ==HTTP_MSG_DONE && req->content_len > 0)
				process_body(req);			
			
		}
		else
			req->msg_state = HTTP_MSG_RQMETH;			
	}
	
}


void init_http_msg(http_waf_msg *req)
{
	req->rule_id      = 0;
	req->severity     = 0;
	req->white_url    = 0;
	req->black_url    = 0;
	req->no_www_file  = 0;
	req->logcenter    = 0;
	req->err_state    = 0;
	req->uri          = NULL;
	req->host         = NULL;
	req->req_file     = NULL;
	req->req_dir      = NULL;
	req->log_msg      = NULL;
	req->str_matched  = NULL;
}

int check_action(http_waf_msg *req)
{

	if(req->white_url == 1)
		return ALLOW;

	if(gvar.err_is_attack == 1 && req->err_state == HTTP_MSG_ERROR)
	{
		snprintf(http_log_msg,sizeof(http_log_msg)-1,"HTTP PROTOCOL ERROR ,Perhaps  Attack.");			
		req->log_msg     = http_log_msg;
		req->rule_id     = ATK_ERR_HEAD;
	}	
	

	if(gvar.action == ALERT)
	{
		if(req->rule_id > 0)
			return ALERT;
		if(req->black_url == 1)
			return ALERT;
		if(req->no_www_file == 1)
			return ALERT;
		
		
	}	

	
	if(gvar.action == DROP)
	{
		if(req->rule_id > 0)
			return DROP;
		if(req->black_url == 1)
			return DROP;
		if(req->no_www_file == 1)
			return DROP;		
		
	}	

	
	return ALLOW;

}


int process_http(const char *buf,int len,http_waf_msg *req)
{
	
	//printf("len=%d state=%d\n",len,req->msg_state);
	switch(req->msg_state)
	{
		/*first http header msg */
		case HTTP_MSG_RQBEFORE:
			memcpy(req->buf,buf,len);
			req->file_size += len;
			req->pos = len;
			req->buf[len] = '\0';
			init_http_msg(req);
			process_header(req);
			break;

		/*header data not over*/
		case HTTP_MSG_RQMETH: 
			if((req->pos + len) > MAX_HTTP_HEADER_SIZE)
				return check_action(req);
			memcpy(req->buf + req->pos,buf,len);
			req->pos += len;
			req->buf[req->pos] = '\0';
			process_header(req);				
			break;
		/*body not over*/
		case HTTP_MSG_BODY: 
			req->file_size += len;
			if(req->file_size >= req->content_len && req->content_len >= MAX_HTTP_HEADER_SIZE)
			{				
				req->msg_state = HTTP_MSG_DONE;
				req->file_size = 0;
			}
				
			if((req->pos + len) > MAX_HTTP_HEADER_SIZE)
			{					
				process_body(req);
				req->only_once = 1;
				
				return check_action(req);
			}	
			memcpy(req->buf + req->pos,buf,len);
			req->pos += len;
			req->buf[req->pos] = '\0';
			req->req_body_size = req->pos - req->req_hdr_size;
			if(req->req_body_size >= req->content_len)
			{
				req->msg_state = HTTP_MSG_DONE;
				process_body(req);	
			}
			break;	
		/*keep alive or h2*/
		case HTTP_MSG_DONE:
			memcpy(req->buf,buf,len);	
			req->file_size += len;
			req->pos = len;
			req->buf[len] = '\0';
			init_http_msg(req);
			process_header(req);
			break;
		
			
		default:
			break;
	}

	return check_action(req);
	
	
}


