/* SPDX-License-Identifier: GPL-2.0 */
#ifndef PARSE_CTX_H
#define PARSE_CTX_H 1

// There are fixes that need to land upstream before we can use libbpf's headers,
// for now use our copy unconditionally, since the data structures at this point
// are exactly the same, no problem.
//#ifdef HAVE_LIBBPF_SUPPORT
//#include <bpf/hashmap.h>
//#else
#include "util/hashmap.h"
//#endif

struct metric_ref;

struct expr_id {
	char		*id;
	struct expr_id	*parent;
};

struct expr_parse_ctx {
	struct hashmap	*ids;
	struct expr_id	*parent;
};

struct expr_id_data;

struct expr_scanner_ctx {
	int runtime;
};

struct hashmap *ids__new(void);
void ids__free(struct hashmap *ids);
int ids__insert(struct hashmap *ids, const char *id, struct expr_id *parent);
/*
 * Union two sets of ids (hashmaps) and construct a third, freeing ids1 and
 * ids2.
 */
struct hashmap *ids__union(struct hashmap *ids1, struct hashmap *ids2);

struct expr_parse_ctx *expr__ctx_new(void);
void expr__ctx_clear(struct expr_parse_ctx *ctx);
void expr__ctx_free(struct expr_parse_ctx *ctx);

void expr__del_id(struct expr_parse_ctx *ctx, const char *id);
int expr__add_id(struct expr_parse_ctx *ctx, const char *id);
int expr__add_id_val(struct expr_parse_ctx *ctx, const char *id, double val);
int expr__add_ref(struct expr_parse_ctx *ctx, struct metric_ref *ref);
int expr__get_id(struct expr_parse_ctx *ctx, const char *id,
		 struct expr_id_data **data);
int expr__resolve_id(struct expr_parse_ctx *ctx, const char *id,
		     struct expr_id_data **datap);

int expr__parse(double *final_val, struct expr_parse_ctx *ctx,
		const char *expr, int runtime);

int expr__find_ids(const char *expr, const char *one,
		struct expr_parse_ctx *ids, int runtime);

double expr_id_data__value(const struct expr_id_data *data);
struct expr_id *expr_id_data__parent(struct expr_id_data *data);

#endif
