// SPDX-License-Identifier: GPL-2.0
#include <stdbool.h>
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "metricgroup.h"
#include "debug.h"
#include "expr.h"
#include "expr-bison.h"
#include "expr-flex.h"
#include <linux/kernel.h>
#include <linux/zalloc.h>
#include <ctype.h>

#ifdef PARSER_DEBUG
extern int expr_debug;
#endif

struct expr_id_data {
	union {
		double val;
		struct {
			double val;
			const char *metric_name;
			const char *metric_expr;
		} ref;
		struct expr_id	*parent;
	};

	enum {
		/* Holding a double value. */
		EXPR_ID_DATA__VALUE,
		/* Reference to another metric. */
		EXPR_ID_DATA__REF,
		/* A reference but the value has been computed. */
		EXPR_ID_DATA__REF_VALUE,
		/* A parent is remembered for the recursion check. */
		EXPR_ID_DATA__PARENT,
	} kind;
};

static size_t key_hash(const void *key, void *ctx __maybe_unused)
{
	const char *str = (const char *)key;
	size_t hash = 0;

	while (*str != '\0') {
		hash *= 31;
		hash += *str;
		str++;
	}
	return hash;
}

static bool key_equal(const void *key1, const void *key2,
		    void *ctx __maybe_unused)
{
	return !strcmp((const char *)key1, (const char *)key2);
}

struct hashmap *ids__new(void)
{
	return hashmap__new(key_hash, key_equal, NULL);
}

void ids__free(struct hashmap *ids)
{
	struct hashmap_entry *cur;
	size_t bkt;

	if (ids == NULL)
		return;

	hashmap__for_each_entry(ids, cur, bkt) {
		free((char *)cur->key);
		free(cur->value);
	}

	hashmap__free(ids);
}

int ids__insert(struct hashmap *ids, const char *id,
		struct expr_id *parent)
{
	struct expr_id_data *data_ptr = NULL, *old_data = NULL;
	char *old_key = NULL;
	int ret;

	data_ptr = malloc(sizeof(*data_ptr));
	if (!data_ptr)
		return -ENOMEM;

	data_ptr->parent = parent;
	data_ptr->kind = EXPR_ID_DATA__PARENT;

	ret = hashmap__set(ids, id, data_ptr,
			   (const void **)&old_key, (void **)&old_data);
	if (ret)
		free(data_ptr);
	free(old_key);
	free(old_data);
	return ret;
}

struct hashmap *ids__union(struct hashmap *ids1, struct hashmap *ids2)
{
	size_t bkt;
	struct hashmap_entry *cur;
	int ret;
	struct expr_id_data *old_data = NULL;
	char *old_key = NULL;

	if (!ids1)
		return ids2;

	if (!ids2)
		return ids1;

	if (hashmap__size(ids1) <  hashmap__size(ids2)) {
		struct hashmap *tmp = ids1;

		ids1 = ids2;
		ids2 = tmp;
	}
	hashmap__for_each_entry(ids2, cur, bkt) {
		ret = hashmap__set(ids1, cur->key, cur->value,
				(const void **)&old_key, (void **)&old_data);
		free(old_key);
		free(old_data);

		if (ret) {
			hashmap__free(ids1);
			hashmap__free(ids2);
			return NULL;
		}
	}
	hashmap__free(ids2);
	return ids1;
}

/* Caller must make sure id is allocated */
int expr__add_id(struct expr_parse_ctx *ctx, const char *id)
{
	return ids__insert(ctx->ids, id, ctx->parent);
}

/* Caller must make sure id is allocated */
int expr__add_id_val(struct expr_parse_ctx *ctx, const char *id, double val)
{
	struct expr_id_data *data_ptr = NULL, *old_data = NULL;
	char *old_key = NULL;
	int ret;

	data_ptr = malloc(sizeof(*data_ptr));
	if (!data_ptr)
		return -ENOMEM;
	data_ptr->val = val;
	data_ptr->kind = EXPR_ID_DATA__VALUE;

	ret = hashmap__set(ctx->ids, id, data_ptr,
			   (const void **)&old_key, (void **)&old_data);
	if (ret)
		free(data_ptr);
	free(old_key);
	free(old_data);
	return ret;
}

int expr__add_ref(struct expr_parse_ctx *ctx, struct metric_ref *ref)
{
	struct expr_id_data *data_ptr = NULL, *old_data = NULL;
	char *old_key = NULL;
	char *name, *p;
	int ret;

	data_ptr = zalloc(sizeof(*data_ptr));
	if (!data_ptr)
		return -ENOMEM;

	name = strdup(ref->metric_name);
	if (!name) {
		free(data_ptr);
		return -ENOMEM;
	}

	/*
	 * The jevents tool converts all metric expressions
	 * to lowercase, including metric references, hence
	 * we need to add lowercase name for metric, so it's
	 * properly found.
	 */
	for (p = name; *p; p++)
		*p = tolower(*p);

	/*
	 * Intentionally passing just const char pointers,
	 * originally from 'struct pmu_event' object.
	 * We don't need to change them, so there's no
	 * need to create our own copy.
	 */
	data_ptr->ref.metric_name = ref->metric_name;
	data_ptr->ref.metric_expr = ref->metric_expr;
	data_ptr->kind = EXPR_ID_DATA__REF;

	ret = hashmap__set(ctx->ids, name, data_ptr,
			   (const void **)&old_key, (void **)&old_data);
	if (ret)
		free(data_ptr);

	pr_debug2("adding ref metric %s: %s\n",
		  ref->metric_name, ref->metric_expr);

	free(old_key);
	free(old_data);
	return ret;
}

int expr__get_id(struct expr_parse_ctx *ctx, const char *id,
		 struct expr_id_data **data)
{
	return hashmap__find(ctx->ids, id, (void **)data) ? 0 : -1;
}

int expr__resolve_id(struct expr_parse_ctx *ctx, const char *id,
		     struct expr_id_data **datap)
{
	struct expr_id_data *data;

	if (expr__get_id(ctx, id, datap) || !*datap) {
		pr_debug("%s not found\n", id);
		return -1;
	}

	data = *datap;

	switch (data->kind) {
	case EXPR_ID_DATA__VALUE:
		pr_debug2("lookup(%s): val %f\n", id, data->val);
		break;
	case EXPR_ID_DATA__PARENT:
		pr_debug2("lookup(%s): parent %s\n", id, data->parent->id);
		break;
	case EXPR_ID_DATA__REF:
		pr_debug2("lookup(%s): ref metric name %s\n", id,
			data->ref.metric_name);
		pr_debug("processing metric: %s ENTRY\n", id);
		data->kind = EXPR_ID_DATA__REF_VALUE;
		if (expr__parse(&data->ref.val, ctx, data->ref.metric_expr, 1)) {
			pr_debug("%s failed to count\n", id);
			return -1;
		}
		pr_debug("processing metric: %s EXIT: %f\n", id, data->val);
		break;
	case EXPR_ID_DATA__REF_VALUE:
		pr_debug2("lookup(%s): ref val %f metric name %s\n", id,
			data->ref.val, data->ref.metric_name);
		break;
	default:
		assert(0);  /* Unreachable. */
	}

	return 0;
}

void expr__del_id(struct expr_parse_ctx *ctx, const char *id)
{
	struct expr_id_data *old_val = NULL;
	char *old_key = NULL;

	hashmap__delete(ctx->ids, id,
			(const void **)&old_key, (void **)&old_val);
	free(old_key);
	free(old_val);
}

struct expr_parse_ctx *expr__ctx_new(void)
{
	struct expr_parse_ctx *ctx;

	ctx = malloc(sizeof(struct expr_parse_ctx));
	if (!ctx)
		return NULL;

	ctx->ids = hashmap__new(key_hash, key_equal, NULL);
	ctx->parent = NULL;
	return ctx;
}

void expr__ctx_clear(struct expr_parse_ctx *ctx)
{
	struct hashmap_entry *cur;
	size_t bkt;

	hashmap__for_each_entry(ctx->ids, cur, bkt) {
		free((char *)cur->key);
		free(cur->value);
	}
	hashmap__clear(ctx->ids);
}

void expr__ctx_free(struct expr_parse_ctx *ctx)
{
	struct hashmap_entry *cur;
	size_t bkt;

	hashmap__for_each_entry(ctx->ids, cur, bkt) {
		free((char *)cur->key);
		free(cur->value);
	}
	hashmap__free(ctx->ids);
	free(ctx);
}

static int
__expr__parse(double *val, struct expr_parse_ctx *ctx, const char *expr,
	      bool compute_ids, int runtime)
{
	struct expr_scanner_ctx scanner_ctx = {
		.runtime = runtime,
	};
	YY_BUFFER_STATE buffer;
	void *scanner;
	int ret;

	pr_debug2("parsing metric: %s\n", expr);

	ret = expr_lex_init_extra(&scanner_ctx, &scanner);
	if (ret)
		return ret;

	buffer = expr__scan_string(expr, scanner);

#ifdef PARSER_DEBUG
	expr_debug = 1;
	expr_set_debug(1, scanner);
#endif

	ret = expr_parse(val, ctx, compute_ids, scanner);

	expr__flush_buffer(buffer, scanner);
	expr__delete_buffer(buffer, scanner);
	expr_lex_destroy(scanner);
	return ret;
}

int expr__parse(double *final_val, struct expr_parse_ctx *ctx,
		const char *expr, int runtime)
{
	return __expr__parse(final_val, ctx, expr, /*compute_ids=*/false, runtime) ? -1 : 0;
}

int expr__find_ids(const char *expr, const char *one,
		   struct expr_parse_ctx *ctx, int runtime)
{
	int ret = __expr__parse(NULL, ctx, expr, /*compute_ids=*/true, runtime);

	if (one)
		expr__del_id(ctx, one);

	return ret;
}

double expr_id_data__value(const struct expr_id_data *data)
{
	if (data->kind == EXPR_ID_DATA__VALUE)
		return data->val;
	assert(data->kind == EXPR_ID_DATA__REF_VALUE);
	return data->ref.val;
}

struct expr_id *expr_id_data__parent(struct expr_id_data *data)
{
	assert(data->kind == EXPR_ID_DATA__PARENT);
	return data->parent;
}
