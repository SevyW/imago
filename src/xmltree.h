// A small, tolerant XML tree for theme.xml files: elements, attributes and
// text, in document order. Attribute and text bytes are kept verbatim (still
// escaped), so untouched parts are written back exactly as they were read.
#pragma once

#include <stddef.h>

typedef struct XmlNode {
	char *name;
	char *attrs; // raw attribute text after the name, e.g. ` format-ver="01.00"`
	char *text;  // raw (escaped) text content; NULL for elements with children
	int origin;  // which theme the node came from (see themedoc.h)
	struct XmlNode *child, *next;
} XmlNode;

// Parses a document and returns its root element, or NULL if there is none.
// Comments, processing instructions, a BOM and stray text are skipped.
XmlNode *xml_parse(const char *text);
void xml_free(XmlNode *node);
// Serialises with a declaration and two-space indentation. Caller frees.
char *xml_serialize(const XmlNode *root);

XmlNode *xml_new(const char *name, const char *text);
XmlNode *xml_clone(const XmlNode *node);
// Direct child by name, or a '/'-separated path of children ("HomeProperty/m_bgParam").
XmlNode *xml_find(XmlNode *parent, const char *path);
void xml_append(XmlNode *parent, XmlNode *child);
// Unlinks a direct child and frees it. Returns 0 if it was found.
int xml_remove(XmlNode *parent, XmlNode *child);
// Replaces `old` (a direct child of parent) with `replacement`, freeing `old`.
void xml_replace(XmlNode *parent, XmlNode *old, XmlNode *replacement);
// Sets text content from an unescaped string (escaping it).
void xml_set_text(XmlNode *node, const char *text);
// Copies the unescaped, trimmed text content into out.
void xml_get_text(const XmlNode *node, char *out, size_t size);
