#ifndef XH_TYPECLASS_H
#define XH_TYPECLASS_H
/* Standalone, bounded aggregate classification mechanism. Not call lowering.
 * Leaf kind IDs are package-defined. Group edges project children at explicit
 * byte offsets; union alternatives use overlapping offsets. Arrays can share
 * child nodes through repeated edges. No ownership of graph memory. */
#define XH_TYPECLASS_MAX_NODES 128
#define XH_TYPECLASS_MAX_EDGES 256
#define XH_TYPECLASS_MAX_DEPTH 32
#define XH_TYPECLASS_MAX_VISITS 1024
#define XH_TYPECLASS_MAX_FRAGMENTS 2
#define XH_TYPECLASS_GROUP 0
#define XH_TYPECLASS_LEAF 1
typedef struct {int form,kind;unsigned size,alignment,first_edge,edge_count;} XhTypeclassNode;
typedef struct {unsigned node,offset;} XhTypeclassEdge;
typedef struct {const XhTypeclassNode *nodes;unsigned node_count;const XhTypeclassEdge *edges;unsigned edge_count;} XhTypeclassGraph;
typedef struct {unsigned offset,size;int class_id;} XhTypeclassFragment;
typedef struct {int memory;unsigned count;XhTypeclassFragment fragments[XH_TYPECLASS_MAX_FRAGMENTS];} XhTypeclassResult;
/* 0 = valid classification, including MEMORY; -1 = malformed/unsupported.
 * Output is cleared on failure. Rules use shared XHTEMPLATE target/prefix;
 * caller configures its path (e.g. x86_64.aggregate) with xh_template_set_path.
 * One parsed rule cache tracks template generation; same-path mutation needs
 * xh_template_release. Release below frees only this classifier's cache. */
/* Configure an isolated target.aggregate template alias (target <=21 chars).
 * Standalone: loading this alias evicts other template cache pointers. */
int xh_typeclass_set_path(const char *target,const char *path);
int xh_typeclass_select(const char *target,const char *prefix,const XhTypeclassGraph *graph,unsigned root,XhTypeclassResult *out);
void xh_typeclass_release(void);
#endif
