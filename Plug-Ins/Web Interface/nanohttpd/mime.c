#include <string.h>
#include <stdio.h>
#include "nanohttpd.h"

static void mime_header_free( void *_me ) {
	mime_header_t *me = (mime_header_t *) _me;
	if( ! me ) return;
	if( me -> params ) me -> params -> delete( me -> params, 0, 0, NULL );
	free( me );
}

static void mime_part_free( void *_me ) {
	mime_part_t *me = (mime_part_t *) _me;
	if( ! me ) return;
	if( me -> headers ) me -> headers -> delete_func( me -> headers, mime_header_free );
	free( me );
}

static void mime_msg_free( void *_me ) {
	mime_message_t *me = (mime_message_t *) _me;
	if( ! me ) return;
	if( me -> parts ) me -> parts -> delete_func( me -> parts, mime_part_free );
	free( me );
}

#define MIME_END_OF_PART 0
#define MIME_READ_NEXT_PART 1
#define MIME_ERR 2

#define NEXT_LINE( ptr, buflen ) if(( *ptr ) =='\n' ) {( ptr )++;( buflen )--;}

static int mime_parse_part( mime_message_t *msg, char * *content, unsigned long *buflen, char *boundary ) {
	// move the cursor to the beginning of this part 
	char search[255];
	snprintf( search, 255, "--%s", boundary );

	char *tmp = strstr( *content, search );
	if( ! tmp ) {
#ifdef LOG_HTTP
		http_log( LOG_ERR, "No such part !!( ='%s' ), ct='%s'", boundary, *content );
#endif
		return MIME_ERR;
	}

	*buflen -= ( tmp - *content );
	*content = tmp;

	char *line = strsep( content, "\r\n" );
	*buflen -= strlen( line ) + 1 ;

	NEXT_LINE( *content, *buflen )

	// line must be either the beginning of a new part( --boundary )
	// or then end of a part( --boundary-- )

	snprintf( search, 255, "--%s--", boundary );
	if( strcmp( line, search ) ==0 )
		return MIME_END_OF_PART;

	// so, this is a new part... Be sure to have something to parse
	if( *buflen <= 0 ) return MIME_ERR;

	mime_part_t *me = (mime_part_t *) malloc( sizeof( mime_part_t ) );
	me -> boundary = boundary;
	me -> headers = list_new();

	// parse the mime headers 
	line = strsep( content, "\r\n" );
	*buflen -= strlen( line ) +1 ;

	while( strlen( line ) > 1 ) {
		tmp = line;
		char *header_value = NULL;
		char *header_name = strsep( &tmp, ":\r\n" );
		if( tmp ) header_value = strsep( &tmp ,";\r\n" );
		if( header_value ) {
			mime_header_t *header = (mime_header_t *) malloc( sizeof( mime_header_t ) );
			while( *header_value == ' ' ) header_value++; // left trim

			header -> name = header_name;
			header -> value = header_value;
			header -> params = NULL;

			// this header has some parameters
			// try to parse them

			if( tmp ) {
				header -> params = hash_new();

				while( *tmp == ' ' ) tmp++; // left trim
				while( tmp ) {
					char *param_name = strsep( &tmp, "=" );
					while( *param_name == ' ' ) param_name++; // left trim
					if( param_name ) {
						char *param_value = strsep( &tmp, ";\r\n" );
						if( param_value ) {
							// remove leading & trailing \", if any							
							if( *param_value == '"' ) {
								param_value++;
								param_value = strsep( &param_value, "\"" );
							} 

							header -> params -> set( header -> params, param_name, param_value );
						}
					}
				}
			}

			me -> headers -> add( me -> headers, header );
		}

		NEXT_LINE( *content, *buflen );

		line = strsep( content, "\r\n" );
		*buflen -= strlen( line ) +1;
	}

	// empty line between header( s ) and body
	if( **content =='\r' )( *content )++;
	if( **content =='\n' )( *content )++;

	msg -> parts -> add( msg -> parts, me );

	// test if we have a multipart/... message.
	// if yes, we need to parse it by re-calling mime_parse_part
	char *next_boundary;
	
	// search the content-type header
	mime_header_t *header = (mime_header_t *) me -> headers -> first( me -> headers );
	while( header ) {
		if( strcasestr( header -> name, "content-type" ) ) break;
		header = (mime_header_t *) me -> headers -> next( me -> headers );
	}

	if( ! header ) goto parse_part; // no content-type header in this part

	if( strcasestr( header -> value, "multipart/" ) ) {
		next_boundary = NULL;
		if( header -> params ) next_boundary = (char *)header -> params -> get( header -> params, "boundary" );
		if( ! next_boundary ) {
#ifdef LOG_HTTP
			http_log( LOG_ERR, "Bad message! Boundary not specified!" );
#endif
			return MIME_ERR;
		}

		while( mime_parse_part( msg, content, buflen, next_boundary ) == MIME_READ_NEXT_PART );

		return MIME_READ_NEXT_PART;
	}

	// this is not a multipart/ part..
	// lookup the end of this part.
	// this can be either:
	// - the end of this boundary( --boundary-- )
	// - a new part with the same boundary( --boundary )

parse_part:
	me -> begin = *content;		
	snprintf( search, 255, "--%s", boundary ); 
	int len = 0;

	while( *buflen > 0 ) {
		if( *content == NULL ) {
#ifdef LOG_HTTP
			http_log( LOG_DEBUG,"NULL pointer detected" );
#endif
			return MIME_ERR;
		}

		if( **content =='-' && memcmp( *content, search, strlen( search ) ) == 0 ) {
			// we found the end of the part
			break;
		}

		(*content)++;
		(*buflen)--;
		len++;
	}

	if( (*buflen) > 0 ) {
#ifdef LOG_HTTP
		http_log( LOG_DEBUG, "unexpected end of part, (rest = %s), bl = %lu", *content, *buflen );
#endif
		return MIME_ERR;
	}

	me -> length = len;		

	return MIME_READ_NEXT_PART;
}

mime_message_t *mime_parse_message( char *buf, unsigned long *buflen, char *boundary ) {
	mime_message_t *me = (mime_message_t *) malloc( sizeof( mime_message_t ) );
	me -> delete = mime_msg_free;

	char search[255];
	snprintf( search, 255, "--%s", boundary );
	char *content = strstr( buf, search );
	*buflen -= ( content - buf );
	if( ! content ) {
#ifdef LOG_HTTP
		http_log( LOG_ERR, "Mime message invalid." );
#endif
		free( me ); return NULL;
	}

	me -> parts = list_new();
	while( mime_parse_part( me, &content, buflen, boundary ) == MIME_READ_NEXT_PART ) {}

	return me;
}
