#include "xmltree.h"

#include "psvt.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static char *dup_range(const char *s, size_t n) {
	char *out = malloc(n + 1);
	if (out) {
		memcpy(out, s, n);
		out[n] = '\0';
	}
	return out;
}

static char *dup_str(const char *s) {
	return s ? dup_range(s, strlen(s)) : NULL;
}

XmlNode *xml_new(const char *name, const char *text) {
	XmlNode *n = calloc(1, sizeof(*n));
	if (!n)
		return NULL;
	n->name = dup_str(name);
	n->attrs = dup_str("");
	if (text)
		xml_set_text(n, text);
	return n;
}

void xml_free(XmlNode *node) {
	while (node) {
		XmlNode *next = node->next;
		xml_free(node->child);
		free(node->name);
		free(node->attrs);
		free(node->text);
		free(node);
		node = next;
	}
}

// --- parsing --------------------------------------------------------------------

typedef struct {
	const char *p;
	int depth;
} Parser;

// Skips whitespace, comments, processing instructions, doctype and stray text
// until the next '<' that starts an element or a closing tag.
static void skip_misc(Parser *ps) {
	for (;;) {
		while (*ps->p && *ps->p != '<')
			ps->p++;
		if (!*ps->p)
			return;
		if (strncmp(ps->p, "<!--", 4) == 0) {
			const char *end = strstr(ps->p + 4, "-->");
			ps->p = end ? end + 3 : ps->p + strlen(ps->p);
		} else if (ps->p[1] == '?' || ps->p[1] == '!') {
			const char *end = strchr(ps->p, '>');
			ps->p = end ? end + 1 : ps->p + strlen(ps->p);
		} else {
			return;
		}
	}
}

static XmlNode *parse_element(Parser *ps);

static void parse_children(Parser *ps, XmlNode *node) {
	const char *text_start = ps->p;
	XmlNode **tail = &node->child;
	for (;;) {
		const char *lt = ps->p;
		while (*lt && *lt != '<')
			lt++;
		if (!*lt) {
			ps->p = lt;
			break;
		}
		if (lt[1] == '/') { // our closing tag (or a stray one: close anyway)
			if (!node->child) {
				// Leaf: keep the text exactly as written, minus surrounding blanks.
				const char *a = text_start, *b = lt;
				while (a < b && isspace((unsigned char)*a))
					a++;
				while (b > a && isspace((unsigned char)b[-1]))
					b--;
				node->text = dup_range(a, (size_t)(b - a));
			}
			const char *gt = strchr(lt, '>');
			ps->p = gt ? gt + 1 : lt + strlen(lt);
			return;
		}
		ps->p = lt;
		if (strncmp(lt, "<!--", 4) == 0 || lt[1] == '?' || lt[1] == '!') {
			skip_misc(ps);
			continue;
		}
		XmlNode *child = parse_element(ps);
		if (!child)
			break;
		*tail = child;
		tail = &child->next;
	}
}

static XmlNode *parse_element(Parser *ps) {
	if (*ps->p != '<' || ps->depth > 64)
		return NULL;
	const char *name = ps->p + 1;
	const char *q = name;
	while (*q && !isspace((unsigned char)*q) && *q != '>' && *q != '/')
		q++;
	if (q == name)
		return NULL;
	XmlNode *node = calloc(1, sizeof(*node));
	if (!node)
		return NULL;
	node->name = dup_range(name, (size_t)(q - name));
	const char *attrs = q;
	// Find the end of the start tag, honouring quotes in attribute values.
	char quote = 0;
	while (*q && (quote || (*q != '>'))) {
		if (quote && *q == quote)
			quote = 0;
		else if (!quote && (*q == '"' || *q == '\''))
			quote = *q;
		q++;
	}
	if (!*q) {
		xml_free(node);
		return NULL;
	}
	int self_closing = q > attrs && q[-1] == '/';
	const char *attrs_end = self_closing ? q - 1 : q;
	while (attrs_end > attrs && isspace((unsigned char)attrs_end[-1]))
		attrs_end--;
	node->attrs = dup_range(attrs, (size_t)(attrs_end - attrs));
	ps->p = q + 1;
	if (self_closing) {
		node->text = dup_str("");
		return node;
	}
	ps->depth++;
	parse_children(ps, node);
	ps->depth--;
	return node;
}

XmlNode *xml_parse(const char *text) {
	Parser ps = {text, 0};
	if ((unsigned char)ps.p[0] == 0xEF && (unsigned char)ps.p[1] == 0xBB && (unsigned char)ps.p[2] == 0xBF)
		ps.p += 3;
	skip_misc(&ps);
	return parse_element(&ps);
}

// --- serialising -------------------------------------------------------------------

typedef struct {
	char *data;
	size_t len, cap;
} Buffer;

static void put(Buffer *b, const char *s, size_t n) {
	if (b->len + n + 1 > b->cap) {
		size_t cap = b->cap ? b->cap * 2 : 4096;
		while (cap < b->len + n + 1)
			cap *= 2;
		char *grown = realloc(b->data, cap);
		if (!grown)
			return;
		b->data = grown;
		b->cap = cap;
	}
	memcpy(b->data + b->len, s, n);
	b->len += n;
	b->data[b->len] = '\0';
}

static void puts_(Buffer *b, const char *s) {
	put(b, s, strlen(s));
}

static void write_node(Buffer *b, const XmlNode *n, int depth) {
	for (int i = 0; i < depth; i++)
		puts_(b, "  ");
	puts_(b, "<");
	puts_(b, n->name);
	puts_(b, n->attrs ? n->attrs : "");
	puts_(b, ">");
	if (n->child) {
		puts_(b, "\n");
		for (const XmlNode *c = n->child; c; c = c->next)
			write_node(b, c, depth + 1);
		for (int i = 0; i < depth; i++)
			puts_(b, "  ");
	} else if (n->text) {
		puts_(b, n->text);
	}
	puts_(b, "</");
	puts_(b, n->name);
	puts_(b, ">\n");
}

char *xml_serialize(const XmlNode *root) {
	Buffer b = {0};
	puts_(&b, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n");
	if (root)
		write_node(&b, root, 0);
	return b.data;
}

// --- editing -----------------------------------------------------------------------

XmlNode *xml_clone(const XmlNode *node) {
	if (!node)
		return NULL;
	XmlNode *copy = calloc(1, sizeof(*copy));
	if (!copy)
		return NULL;
	copy->name = dup_str(node->name);
	copy->attrs = dup_str(node->attrs);
	copy->text = dup_str(node->text);
	copy->origin = node->origin;
	XmlNode **tail = &copy->child;
	for (const XmlNode *c = node->child; c; c = c->next) {
		*tail = xml_clone(c);
		if (*tail)
			tail = &(*tail)->next;
	}
	return copy;
}

XmlNode *xml_find(XmlNode *parent, const char *path) {
	while (parent && *path) {
		const char *slash = strchr(path, '/');
		size_t len = slash ? (size_t)(slash - path) : strlen(path);
		XmlNode *found = NULL;
		for (XmlNode *c = parent->child; c; c = c->next) {
			if (strlen(c->name) == len && strncmp(c->name, path, len) == 0) {
				found = c;
				break;
			}
		}
		parent = found;
		path = slash ? slash + 1 : path + len;
	}
	return parent;
}

void xml_append(XmlNode *parent, XmlNode *child) {
	if (!parent || !child)
		return;
	XmlNode **tail = &parent->child;
	while (*tail)
		tail = &(*tail)->next;
	*tail = child;
	child->next = NULL;
	free(parent->text);
	parent->text = NULL;
}

int xml_remove(XmlNode *parent, XmlNode *child) {
	for (XmlNode **link = &parent->child; *link; link = &(*link)->next) {
		if (*link == child) {
			*link = child->next;
			child->next = NULL;
			xml_free(child);
			return 0;
		}
	}
	return -1;
}

void xml_replace(XmlNode *parent, XmlNode *old, XmlNode *replacement) {
	for (XmlNode **link = &parent->child; *link; link = &(*link)->next) {
		if (*link == old) {
			replacement->next = old->next;
			*link = replacement;
			old->next = NULL;
			xml_free(old);
			return;
		}
	}
	xml_append(parent, replacement);
}

void xml_set_text(XmlNode *node, const char *text) {
	size_t n = 0;
	for (const char *p = text; *p; p++)
		n += *p == '&' ? 5 : (*p == '<' || *p == '>') ? 4 : (*p == '"') ? 6 : 1;
	char *out = malloc(n + 1);
	if (!out)
		return;
	char *o = out;
	for (const char *p = text; *p; p++) {
		const char *rep = *p == '&' ? "&amp;" : *p == '<' ? "&lt;" : *p == '>' ? "&gt;" : *p == '"' ? "&quot;" : NULL;
		if (rep) {
			memcpy(o, rep, strlen(rep));
			o += strlen(rep);
		} else {
			*o++ = *p;
		}
	}
	*o = '\0';
	free(node->text);
	node->text = out;
	xml_free(node->child);
	node->child = NULL;
}

void xml_get_text(const XmlNode *node, char *out, size_t size) {
	if (!node || !node->text) {
		if (size)
			out[0] = '\0';
		return;
	}
	psvt_html_text(out, size, node->text, strlen(node->text));
}
