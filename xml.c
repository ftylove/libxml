#include <stdlib.h>
#include <string.h>

#include "xml.h"

#define WHITESPACE " \t\r\n"

#define TAG_ELEMENT_OPEN 1
#define TAG_ELEMENT_CLOSE 2
#define TAG_PI 3
#define TAG_TYPE 4
#define TAG_COMMENT 5
#define TAG_CDATA 6

struct xml_path_segment {
	size_t tag_len;
	struct xml_query_string {
		const char *key;
		size_t key_len;
		const char *value;
		size_t value_len;
		struct xml_query_string *next;
	} *query;
};

struct xml_tag_pattern {
	int type;
	const char *open;
	size_t open_len;
	const char *close;
} xml_tag_patterns[] = {
	{TAG_ELEMENT_OPEN, "<", 1, ">"},
	{TAG_ELEMENT_CLOSE, "</", 2, ">"},
	{TAG_PI, "<?", 2, "?>"},
	{TAG_TYPE, "<!", 2, ">"},
	{TAG_COMMENT, "<!--", 4, "-->"},
	{TAG_CDATA, "<![CDATA[", 9, "]]>"},
	{0, 0, 0, 0}
};

/*****************************************************************************
 * STRING OPERATIONS
 ****************************************************************************/

/**
 * Return length of quoted string respecting escaped characters
 *
 * @param s - first character after quote
 * @param q - quote character
 */
static size_t xml_quotedspn(const char *s, char q) {
	const char *first = s;

	while (*++s) {
		if (*s == '\\') {
			++s;
			continue;
		}

		if (*s == q) {
			return s - first;
		}
	}

	return 0;
}

#ifdef WIN32
/**
 * Copies n bytes of given string
 *
 * @param s - string to copy
 * @param n - number of bytes
 */
static char *strndup(const char *s, size_t n) {
	char *r;

	if (!(r = calloc(n + 1, sizeof(char))) ||
			!memcpy(r, s, n)) {
		return NULL;
	}

	return r;
}
#endif

/**
 * Append string
 *
 * @param dest - address of string to append to
 * @param dest_len - address of length of string
 * @param src - string to append
 * @param src_len - length of string to append
 */
static char *xml_string_append(
		char **dest,
		size_t *dest_len,
		const char *src,
		size_t src_len) {
	char *n;

	if (src_len < 1) {
		return *dest;
	}

	if (!*dest) {
		if ((*dest = strndup(src, src_len))) {
			*dest_len += src_len;
		}
	} else if ((n = realloc(*dest, *dest_len + src_len + 1))) {
		*dest = n;
		n += *dest_len;

		*n = 0;
		strncat(n, src, src_len);

		*dest_len += src_len;
	}

	return *dest;
}

/*****************************************************************************
 * CREATING AND MODIFYING ELEMENTS
 ****************************************************************************/

/**
 * Add child to parent element
 *
 * @param p - parent element
 * @param c - child element
 */
static void xml_element_add(
		struct xml_element *p,
		struct xml_element *c) {
	c->parent = p;

	if (p->first_child) {
		p->last_child->next = c;
		p->last_child = c;
	} else {
		p->first_child = p->last_child = c;
	}
}

/**
 * Create a new element
 *
 * @param parent - parent element
 */
static struct xml_element *xml_element_create(struct xml_element *parent) {
	struct xml_element *e;

	if (!(e = calloc(1, sizeof(struct xml_element)))) {
		return NULL;
	}

	if (parent) {
		xml_element_add(parent, e);
	}

	return e;
}

/*****************************************************************************
 * CREATING AND MODIFYING ATTRIBUTES
 ****************************************************************************/

/**
 * Add attribute to element
 *
 * @param e - element
 * @param a - attribute
 */
static void xml_attribute_add(
		struct xml_element *e,
		struct xml_attribute *a) {
	if (e->first_attribute) {
		e->last_attribute->next = a;
		e->last_attribute = a;
	} else {
		e->first_attribute = e->last_attribute = a;
	}
}

/**
 * Create a new attribute
 *
 * @param parent - parent element
 */
static struct xml_attribute *xml_attribute_create(
		struct xml_element *parent) {
	struct xml_attribute *a;

	if (!(a = calloc(1, sizeof(struct xml_attribute)))) {
		return NULL;
	}

	if (parent) {
		xml_attribute_add(parent, a);
	}

	return a;
}

/*****************************************************************************
 * APPENDING KEY/VALUE
 ****************************************************************************/

/**
 * Append character data to current element
 *
 * @param st - state
 * @param d - begin of data
 * @param l - length of data
 */
static int xml_value_append(struct xml_state *st, const char *d, size_t l) {
	if (!st->length &&
			!(st->current = xml_element_create(st->current))) {
		return -1;
	}

	if (!xml_string_append(
			&st->current->value,
			&st->length,
			d,
			l)) {
		return -1;
	}

	return 0;
}

/**
 * Append tag data to current element
 *
 * @param st - state
 * @param d - begin of data
 * @param l - length of data
 */
static int xml_key_append(struct xml_state *st, const char *d, size_t l) {
	if (!st->current) {
		return -1;
	}

	/* ignore data for closing elements */
	if (st->tag->type == TAG_ELEMENT_CLOSE) {
		return 0;
	}

	/* append start of pattern for special tag types */
	if (!st->length &&
			st->tag->open_len > 1 &&
			!xml_string_append(
				&st->current->key,
				&st->length,
				st->tag->open + 1,
				st->tag->open_len - 1)) {
		return -1;
	}

	if (!xml_string_append(
			&st->current->key,
			&st->length,
			d,
			l)) {
		return -1;
	}

	return 0;
}

/*****************************************************************************
 * PARSING ATTRIBUTES
 ****************************************************************************/

/**
 * Parse attributes
 *
 * @param e - element
 * @param from - first character after tag name
 */
static int xml_parse_attributes(struct xml_element *e, char *from) {
	while (*from) {
		struct xml_attribute *a;
		size_t p;
		char *key = NULL;
		char *value = NULL;
		size_t key_len = 0;
		size_t value_len = 0;

		/* skip leading white space */
		from += strspn(from, WHITESPACE);

		/* search for first character that is not part of a name */
		p = strcspn(from, "="WHITESPACE);

		if (p < 1) {
			break;
		}

		key = from;
		key_len = p;

		/* move after argument name */
		from += p;

		/* skip white space before next control character */
		from += strspn(from, WHITESPACE);

		if (*from == '=') {
			char q = 0;

			/* move after '=' and skip leading white space */
			++from;
			from += strspn(from, WHITESPACE);

			if (!*from) {
				break;
			} else if (*from == '\'') {
				p = xml_quotedspn(++from, (q = '\''));
			} else if (*from == '"') {
				p = xml_quotedspn(++from, (q = '"'));
			} else {
				/* argument data is unqouted */
				p = strcspn(from, WHITESPACE);
			}

			value = from;
			value_len = p;
			from += p;

			if (*from == q) {
				++from;
			}
		}

		if (!(a = xml_attribute_create(e))) {
			return -1;
		}

		if (key) {
			*(key + key_len) = 0;
			a->key = key;
		}

		if (value) {
			*(value + value_len) = 0;
			a->value = value;
		}
	}

	return 0;
}

/*****************************************************************************
 * PARSING ELEMENTS
 ****************************************************************************/

/* forward declarations */
static const char *xml_parse_content(struct xml_state *, const char *);

/**
 * Close element
 *
 * @param st - state
 */
static void xml_close_element(struct xml_state *st) {
	st->current = st->current->parent;
}

/**
 * Close tag
 *
 * @param st - state
 */
static void xml_close_tag(struct xml_state *st) {
	st->tag = NULL;
	st->length = 0;
	st->cursor = 0;
	st->empty = 0;
	st->parser = xml_parse_content;
}

/**
 * Parse end of tag name for empty element marker
 *
 * @param st - state
 */
static void xml_check_empty(struct xml_state *st) {
	char *p = st->current->key + strlen(st->current->key) - 1;

	for (; p >= st->current->key; --p) {
		/* ignore trailing white space; this isn't required
		 * by the spec but probably better to have */
		if (strchr(WHITESPACE, *p)) {
			continue;
		}

		if (*p == '/') {
			*p = 0;
			st->empty = 1;
		}

		break;
	}
}

/**
 * Parse tag name
 *
 * @param st - state
 */
static int xml_parse_tag_name(struct xml_state *st) {
	char *p = st->current->key;

	xml_check_empty(st);
	p += strcspn(p, WHITESPACE);

	if (!*p) {
		/* key ends with the name */
		return 0;
	}

	/* terminate name and skip further white space */
	*p++ = 0;
	p += strspn(p, WHITESPACE);

	if (*p && xml_parse_attributes(st->current, p)) {
		return -1;
	}

	return 0;
}

/**
 * Parse tag until terminating pattern
 *
 * @param st - state
 * @param d - XML data
 */
static const char *xml_parse_tag_body(struct xml_state *st, const char *d) {
	while (*d) {
		const char *m = NULL;

		if (!st->cursor) {
			/* find first character of terminating pattern */
			m = strchr(d, st->tag->close[0]);
		} else {
			/* find next character of terminating pattern */
			for (;;) {
				if (st->tag->close[st->cursor] == *d) {
					m = d;
				}

				/* break if next character does match or cursor
				 * is at the first character */
				if (m || !st->cursor) {
					break;
				}

				/* append leading part of pattern since it
				 * is data if d doesn't match */
				if (xml_key_append(st, st->tag->close, st->cursor)) {
					return NULL;
				}

				/* if cursor has already moved into a pattern
				 * but d doesn't match, d may still match the
				 * beginning of a pattern */
				st->cursor = 0;
			}

			/* start over if no match was found and cursor
			 * is at first character of the pattern */
			if (!m && !st->cursor) {
				continue;
			}
		}

		if (m) {
			/* put data until this match into tag name */
			if (!st->cursor && xml_key_append(st, d, m - d)) {
				return NULL;
			}

			d = ++m;
			++st->cursor;

			/* terminating pattern complete */
			if (!st->tag->close[st->cursor]) {
				/* append termination pattern for special tag types */
				if (st->cursor > 1 &&
						!xml_string_append(
							&st->current->key,
							&st->length,
							st->tag->close,
							st->cursor - 1)) {
					return NULL;
				}

				if (st->tag->type == TAG_ELEMENT_OPEN &&
						xml_parse_tag_name(st)) {
					return NULL;
				}

				if (st->tag->type != TAG_ELEMENT_OPEN ||
						st->empty) {
					xml_close_element(st);
				}

				xml_close_tag(st);
				break;
			}
		} else {
			/* append all the rest */
			size_t l = strlen(d);

			if (xml_key_append(st, d, l)) {
				return NULL;
			}

			/* move to end of data */
			d += l;

			break;
		}
	}

	return d;
}

/**
 * Parse beginning of a tag to determine its type (and terminating pattern)
 *
 * @param st - state
 * @param d - XML data
 */
static const char *xml_parse_tag_opening(struct xml_state *st,
		const char *d) {
	for (; *d; ++d) {
		struct xml_tag_pattern *p = xml_tag_patterns;

		/* check character against all opening patterns */
		for (;;) {
			for (; p->type; ++p) {
				if (p->open_len > st->cursor &&
						p->open[st->cursor] == *d) {
					break;
				}
			}

			/* break if pattern is found or cursor is at
			 * first character */
			if (p->type || !st->cursor) {
				break;
			}

			/* if cursor has already moved into a pattern but
			 * d doesn't match, d may still match the beginning
			 * of a pattern */
			st->cursor = 0;
		}

		/* if character doesn't match any pattern anymore
		 * take the latest matching pattern */
		if (!p->type) {
			if (!st->tag) {
				return NULL;
			}

			if (st->length > 0) {
				xml_close_element(st);
			}

			st->length = 0;
			st->cursor = 0;
			st->parser = xml_parse_tag_body;

			/* create child element */
			if (st->tag->type != TAG_ELEMENT_CLOSE &&
					!(st->current = xml_element_create(st->current))) {
				return NULL;
			}

			break;
		}

		++st->cursor;
		st->tag = p;
	}

	return d;
}

/**
 * Parse character data of active element
 *
 * @param st - state
 * @param d - XML data
 */
static const char *xml_parse_content(struct xml_state *st, const char *d) {
	const char *end = d + strcspn(d, "<");

	if (end > d && xml_value_append(st, d, end - d)) {
		return NULL;
	}

	if (*end == '<') {
		st->parser = xml_parse_tag_opening;
	}

	return end;
}

/**
 * Parse (next) chunk of a XML document
 *
 * @param st - parsing status
 * @param d - XML chunk
 */
int xml_parse_chunk(struct xml_state *st, const char *d) {
	if (!d) {
		return -1;
	}

	if (!st->root) {
		st->current = st->root = xml_element_create(NULL);
	}

	if (!st->parser) {
		xml_close_tag(st);
	}

	while (*d) {
		if (!(d = st->parser(st, d))) {
			return -1;
		}
	}

	return 0;
}

/**
 * Parse XML document
 *
 * @param data - XML string
 */
struct xml_element *xml_parse(const char *data) {
	struct xml_state st;

	memset(&st, 0, sizeof(st));

	if (!xml_parse_chunk(&st, data) && st.root) {
		return st.root;
	}

	return NULL;
}

/*****************************************************************************
 * FREE MEMORY
 ****************************************************************************/

/**
 * Free XML element tree
 *
 * @param e - root element
 */
void xml_free(struct xml_element *e) {
	struct xml_element *c, *n;

	if (!e) {
		return;
	}

	for (c = e->first_child; c; c = n) {
		n = c->next;
		xml_free(c);
	}

	/* free attributes */
	{
		struct xml_attribute *a, *na;

		for (a = e->first_attribute; a; a = na) {
			na = a->next;

			/* don't free key/value because they're
			 * just pointers into e->key */
			free(a);
		}
	}

	free(e->key);
	free(e->value);
	free(e);
}

/*****************************************************************************
 * ATTRIBUTE LOCATION
 ****************************************************************************/

/**
 * Returns attribute by key
 *
 * @param a - first attribute
 * @param key - attribute key
 */
struct xml_attribute *xml_find_attribute(
		struct xml_attribute *a,
		const char *key) {
	for (; a; a = a->next) {
		if (!strcasecmp(a->key, key)) {
			return a;
		}
	}

	return NULL;
}

/*****************************************************************************
 * ELEMENT LOCATION
 ****************************************************************************/

/**
 * Returns true if element has a matching argument
 *
 * @param e - element
 * @param seg - path segment
 */
static int xml_attribute_match(
		struct xml_element *e,
		struct xml_path_segment *seg) {
	struct xml_attribute *a;
	struct xml_query_string *q;

	if (!e) {
		return 0;
	}

	if (!seg->query) {
		return 1;
	}

	for (q = seg->query; q; q = q->next) {
		for (a = e->first_attribute; a; a = a->next) {
			if (strlen(a->key) == q->key_len &&
					!strncmp(a->key, q->key, q->key_len)) {
				if (!q->value) {
					break;
				}

				if (strlen(a->value) == q->value_len &&
						!strncmp(a->value, q->value, q->value_len)) {
					break;
				}
			}
		}

		if (!a) {
			return 0;
		}
	}

	return 1;
}

/**
 * Add query string to path segment struct
 *
 * @param seg - path segment
 */
static struct xml_query_string *xml_add_query_string(
		struct xml_path_segment *seg) {
	struct xml_query_string *q = calloc(
		1,
		sizeof(struct xml_query_string));

	if (!q) {
		return NULL;
	}

	if (seg->query) {
		q->next = seg->query;
	} else {
		q->next = NULL;
	}

	seg->query = q;

	return q;
}

/**
 * Free allocated query strings
 *
 * @param seg - path segment
 */
static void xml_free_query_strings(struct xml_path_segment *seg) {
	struct xml_query_string *q, *n;

	for (q = seg->query; q; q = n) {
		n = q->next;
		free(q);
	}

	seg->query = NULL;
}

/**
 * Parse first path segment into given structure and return start
 * of next segment
 *
 * @param seg - path segment
 * @param path - slash seperated element path with optional
 *               "?key=value" restriction for attributes
 */
static const char *xml_first_path_segment(
		struct xml_path_segment *seg,
		const char *path) {
	const char *next = strchr(path, '/');
	const char *sep;
	size_t len;

	if (!next) {
		seg->tag_len = strlen(path);
	} else {
		seg->tag_len = next - path;
		++next;
	}

	if ((sep = strchr(path, '?')) &&
			(len = sep - path) < seg->tag_len) {
		const char *stop = path + seg->tag_len;

		seg->tag_len = len;

		do {
			struct xml_query_string *q;

			if (!(q = xml_add_query_string(seg))) {
				break;
			}

			q->key = ++sep;
			q->key_len = strcspn(q->key, "=&/");
			q->value = q->key + q->key_len;

			if (*q->value == '=') {
				q->value_len = strcspn(++q->value, "&/");

				sep = q->value + q->value_len;
			} else {
				sep = q->value;

				q->value = NULL;
				q->value_len = 0;
			}
		} while (*sep && sep < stop);
	} else {
		seg->query = NULL;
	}

	return next;
}

/**
 * Parse last path segment into given structure and return start
 * of that segment
 *
 * @param seg - path segment
 * @param path - slash seperated element path with optional
 *               "?key=value" restriction for attributes
 */
static const char *xml_last_path_segment(
		struct xml_path_segment *seg,
		const char *path,
		const char *prev) {
	const char *p;

	if (!path) {
		return NULL;
	}

	if (!prev) {
		prev = path + strlen(path);
	}

	while (--prev > path && *prev != '/');

	if (*(p = prev) == '/') {
		++p;
	}

	xml_first_path_segment(seg, p);

	return prev;
}

/**
 * Find first matching XML element
 *
 * @param e - root element
 * @param path - slash seperated element path with optional
 *               "?key=value" restriction for attributes
 */
struct xml_element *xml_find(struct xml_element *e, const char *path) {
#define FREE xml_free_query_strings(&seg);
	struct xml_path_segment seg = {0, NULL};
	const char *next;

	if (!path || !*path) {
		return NULL;
	}

	next = xml_first_path_segment(&seg, path);

	for (e = e->first_child; e; e = e->next)
		if (e->key &&
				/* check length first to not match against
				 * words that begin with path */
				strlen(e->key) == seg.tag_len &&
				!strncasecmp(e->key, path, seg.tag_len) &&
				xml_attribute_match(e, &seg)) {
			struct xml_element *c;

			if (!next) {
				FREE
				return e;
			}

			if ((c = xml_find(e, next))) {
				FREE
				return c;
			}
		}

	FREE
	return NULL;
}

/**
 * Find next element
 *
 * @param last - last matched element
 * @param path - slash seperated element path with optional
 *               "?key=value" restriction for attributes, may be NULL
 * @param prev - previous position in path, may be NULL
 */
static struct xml_element *xml_find_next_from(
		struct xml_element *last,
		const char *path,
		const char *prev) {
#define FREE xml_free_query_strings(&seg);
#define FIND(from) \
		for (e = from; e; e = e->next) {\
			if (e->key &&\
				!strcasecmp(e->key, last->key) &&\
				xml_attribute_match(e, &seg)) {\
				FREE\
				return e;\
			}\
		}

	struct xml_element *e;
	struct xml_path_segment seg = {0, NULL};

	if (!last) {
		return NULL;
	}

	if (path) {
		prev = xml_last_path_segment(&seg, path, prev);
	}

	FIND(last->next)

	/* try other branches */
	struct xml_element *p = last->parent;
	/* The temporary variable in for is supported by C99, and if -std=c99 
	is added, the strndup function is not included to strndup cause an error */
	for ( ; p && p->key;) {
		if ((p = xml_find_next_from(p, path, prev))) {
			FIND(p->first_child)
		}
	}

	FREE
	return NULL;
}

/**
 * Find next element
 *
 * @param last - last matched element
 * @param path - slash seperated element path with optional
 *               "?key=value" restriction for attributes, may be NULL
 */
struct xml_element *xml_find_next(
		struct xml_element *last,
		const char *path) {
	return xml_find_next_from(last, path, NULL);
}

/*****************************************************************************
 * CONTENT CONCATENATION
 ****************************************************************************/

/**
 * Calculate size of all child values
 *
 * @param e - element
 */
static size_t xml_content_len(struct xml_element *e) {
	size_t s = 0;

	for (e = e->first_child; e; e = e->next) {
		if (e->value) {
			s += strlen(e->value);
		} else {
			s += xml_content_len(e);
		}
	}

	return s;
}

/**
 * Copy elements into pre-calculated buffer
 *
 * @param e - element
 * @param t - target
 */
static void xml_content_cpy(struct xml_element *e, char **t) {
	for (e = e->first_child; e; e = e->next) {
		if (e->value) {
			strcpy(*t, e->value);
			*t += strlen(e->value);
		} else {
			xml_content_cpy(e, t);
		}
	}
}

/**
 * Return concatenated content of element and all of its children
 *
 * @param e - element
 */
char *xml_content(struct xml_element *e) {
	size_t l;
	char *s;
	char *t;

	if (!e ||
			(l = xml_content_len(e)) < 1 ||
			!(s = malloc(++l))) {
		return NULL;
	}

	t = s;
	xml_content_cpy(e, &t);

	return s;
}

/**
 * Find first element of given path and return its contents or NULL
 * (the returned pointer must be free()'d after use)
 *
 * @param root - root element
 * @param path - element path
 */
char *xml_content_find(struct xml_element *root, const char *path) {
	return xml_content(xml_find(root, path));
}
