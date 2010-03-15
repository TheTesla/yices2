/*
 * Type table and hash consing
 */

#include <string.h>
#include <assert.h>

#include "yices_limits.h"
#include "memalloc.h"
#include "refcount_strings.h"
#include "hash_functions.h"
#include "types.h"



/*
 * Finalizer for typenames in the symbol table. This function is 
 * called when record r is deleted from the symbol table.
 * All symbols must be generated by the clone function, and have
 * a reference counter (cf. refcount_strings.h).
 */
static void typename_finalizer(stbl_rec_t *r) {
  string_decref(r->string);
}


/*
 * Initialize table, with initial size = n.
 */
static void type_table_init(type_table_t *table, uint32_t n) {
  // abort if the size is too large
  if (n >= YICES_MAX_TYPES) {
    out_of_memory();
  }

  table->kind = (uint8_t *) safe_malloc(n * sizeof(uint8_t));
  table->desc = (type_desc_t *) safe_malloc(n * sizeof(type_desc_t));
  table->card = (uint32_t *) safe_malloc(n * sizeof(uint32_t));
  table->flags = (uint8_t *) safe_malloc(n * sizeof(uint8_t));
  table->name = (char **) safe_malloc(n * sizeof(char *));
  
  table->size = n;
  table->nelems = 0;
  table->free_idx = NULL_TYPE;

  init_int_htbl(&table->htbl, 0); // use default size
  init_stbl(&table->stbl, 0);     // default size too

  // install finalizer in the symbol table
  stbl_set_finalizer(&table->stbl, typename_finalizer);

  // don't allocate sup/inf table
  table->sup_tbl = NULL;
  table->inf_tbl = NULL;
}


/*
 * Extend the table: make it 50% larger
 */
static void type_table_extend(type_table_t *table) {
  uint32_t n;

  /*
   * new size = 1.5 * (old_size + 1) approximately
   * this computation can't overflow since old_size < YICES_MAX_TYPE
   * this also ensures that new size > old size (even if old_size <= 1).
   */
  n = table->size + 1;
  n += n >> 1;
  if (n >= YICES_MAX_TYPES) {
    out_of_memory(); 
  }

  table->kind = (uint8_t *) safe_realloc(table->kind, n * sizeof(uint8_t));
  table->desc = (type_desc_t *) safe_realloc(table->desc, n * sizeof(type_desc_t));
  table->card = (uint32_t *) safe_realloc(table->card, n * sizeof(uint32_t));
  table->flags = (uint8_t *) safe_realloc(table->flags, n * sizeof(uint8_t));
  table->name = (char **) safe_realloc(table->name, n * sizeof(char *));

  table->size = n;
}


/*
 * Get a free type id and initializes its name to NULL.
 * The other fields are not initialized.
 */
static type_t allocate_type_id(type_table_t *table) {
  type_t i;

  i = table->free_idx;
  if (i >= 0) {
    table->free_idx = table->desc[i].next;
  } else {
    i = table->nelems;
    table->nelems ++;
    if (i >= table->size) {
      type_table_extend(table);
    }
  }
  table->name[i] = NULL;

  return i;
}


/*
 * Erase type i: free its descriptor and add i to the free list
 */
static void erase_type(type_table_t *table, type_t i) {
  switch (table->kind[i]) {
  case UNUSED_TYPE: // already deleted
  case BOOL_TYPE:
  case INT_TYPE:
  case REAL_TYPE:
    return; // never delete predefined types

  case BITVECTOR_TYPE:
  case SCALAR_TYPE:
  case UNINTERPRETED_TYPE:
    break;

  case TUPLE_TYPE:
  case FUNCTION_TYPE:
    safe_free(table->desc[i].ptr);
    break;
  }

  if (table->name[i] != NULL) {
    string_decref(table->name[i]);
    table->name[i] = NULL;
  }

  table->kind[i] = UNUSED_TYPE;
  table->desc[i].next = table->free_idx;
  table->free_idx = i;
}




/*
 * INTERNAL CACHES
 */

/*
 * Get the sup_table: create and initialize it if needed
 */
static int_hmap2_t *get_sup_table(type_table_t *table) {
  int_hmap2_t *hmap;

  hmap = table->sup_tbl;
  if (hmap == NULL) {
    hmap = (int_hmap2_t *) safe_malloc(sizeof(int_hmap2_t));
    init_int_hmap2(hmap, 0); // default size
    table->sup_tbl = hmap;
  }

  return hmap;
}


/*
 * Get the inf_table: create and initialize it if needed
 */
static int_hmap2_t *get_inf_table(type_table_t *table) {
  int_hmap2_t *hmap;

  hmap = table->inf_tbl;
  if (hmap == NULL) {
    hmap = (int_hmap2_t *) safe_malloc(sizeof(int_hmap2_t));
    init_int_hmap2(hmap, 0); // default size
    table->inf_tbl = hmap;
  }

  return hmap;
}






/*
 * SUPPORT FOR CARD/FLAGS COMPUTATION
 */

/*
 * Build the conjunction of flags for types a[0 ... n-1]
 *
 * In the result we have
 * - finite flag = 1 if a[0] ... a[n-1] are all finite
 * - unit   flag = 1 if a[0] ... a[n-1] are all unit types
 * - exact  flag = 1 if a[0] ... a[n-1] are all small or unit types
 * - max    flag = 1 if a[0] ... a[n-1] are all maximal types
 * - min    flag = 1 if a[0] ... a[n-1] are all minimal types
 */
static uint32_t type_flags_conjunct(type_table_t *table, uint32_t n, type_t *a) {
  uint32_t i, flg;

  flg = UNIT_TYPE_FLAGS;
  for (i=0; i<n; i++) {
    flg &= type_flags(table, a[i]);
  }

  return flg;
}


/*
 * Product of cardinalities of all types in a[0 ... n-1]
 * - return a value > UINT32_MAX if there's an overflow
 */
static uint64_t type_card_product(type_table_t *table, uint32_t n, type_t *a) {
  uint64_t prod;
  uint32_t i;

  prod = 1;
  for (i=0; i<n; i++) {
    prod *= type_card(table, a[i]);
    if (prod > UINT32_MAX) break;
  }
  return prod;
}


/*
 * Compute the cardinality of function type e[0] ... e[n-1] --> r
 * - all types e[0] ... e[n-1] must be small or unit
 * - r must be small
 * - return a value > UINT32_MAX if there's an overflow
 */
static uint64_t fun_type_card(type_table_t *table, uint32_t n, type_t *e, type_t r) {
  uint64_t power, dom;
  uint32_t range;

  dom = type_card_product(table, n, e);  // domain size
  if (dom >= 32) {
    // since the range has size 2 or more
    // power = range^dom does not fit in 32bits
    power = UINT32_MAX+1;
  } else {
    // compute power = range^dom
    // since dom is small we do this the easy way
    range = type_card(table, r);
    assert(2 <= range && dom >= 1);
    power = range;
    while (dom > 1) {
      power *= range;
      if (power > UINT32_MAX) break;
      dom --;
    }
  }

  return power;
}





/*
 * TYPE CREATION
 */

/*
 * Add the three predefined types
 */
static void add_primitive_types(type_table_t *table) {
  type_t i;

  i = allocate_type_id(table);
  assert(i == bool_id);
  table->kind[i] = BOOL_TYPE;
  table->desc[i].ptr = NULL;
  table->card[i] = 2;
  table->flags[i] = SMALL_TYPE_FLAGS;

  i = allocate_type_id(table);
  assert(i == int_id);
  table->kind[i] = INT_TYPE;
  table->desc[i].ptr = NULL;
  table->card[i] = UINT32_MAX;
  table->flags[i] = (INFINITE_TYPE_FLAGS | TYPE_IS_MINIMAL_MASK);

  i = allocate_type_id(table);
  assert(i == real_id);
  table->kind[i] = REAL_TYPE;
  table->desc[i].ptr = NULL;
  table->card[i] = UINT32_MAX;
  table->flags[i] = (INFINITE_TYPE_FLAGS | TYPE_IS_MAXIMAL_MASK);
}




/*
 * Add type (bitvector k) and return its id
 * - k must be positive and no more than YICES_MAX_BVSIZE
 */
static type_t new_bitvector_type(type_table_t *table, uint32_t k) {
  type_t i;

  assert(0 < k && k <= YICES_MAX_BVSIZE);

  i = allocate_type_id(table);
  table->kind[i] = BITVECTOR_TYPE;
  table->desc[i].integer = k;
  if (k < 32) {
    table->card[i] = ((uint32_t) 1) << k;
    table->flags[i] = SMALL_TYPE_FLAGS;
  } else {
    table->card[i] = UINT32_MAX;
    table->flags[i] = LARGE_TYPE_FLAGS;
  }

  return i;
}


/*
 * Add a scalar type and return its id
 * - k = number of elements in the type 
 * - k must be positive.
 */
type_t new_scalar_type(type_table_t *table, uint32_t k) {
  type_t i;

  assert(k > 0);

  i = allocate_type_id(table);
  table->kind[i] = SCALAR_TYPE;
  table->desc[i].integer = k;
  table->card[i] = k;
  if (k == 1) {
    table->flags[i] = UNIT_TYPE_FLAGS;
  } else {
    table->flags[i] = SMALL_TYPE_FLAGS;
  }

  return i;
}


/*
 * Add a new uninterpreted type and return its id
 * - the type is infinite and both minimal and maximal
 */
type_t new_uninterpreted_type(type_table_t *table) {
  type_t i;

  i = allocate_type_id(table);
  table->kind[i] = UNINTERPRETED_TYPE;
  table->desc[i].ptr = NULL;
  table->card[i] = UINT32_MAX;
  table->flags[i] = (INFINITE_TYPE_FLAGS | TYPE_IS_MAXIMAL_MASK | TYPE_IS_MINIMAL_MASK);

  return i;
}


/*
 * Add tuple type: e[0], ..., e[n-1]
 */
static type_t new_tuple_type(type_table_t *table, uint32_t n, type_t *e) {
  tuple_type_t *d;
  uint64_t card;
  type_t i;
  uint32_t j, flag;

  assert(0 < n && n <= YICES_MAX_ARITY);

  d = (tuple_type_t *) safe_malloc(sizeof(tuple_type_t) + n * sizeof(type_t));
  d->nelem = n;
  for (j=0; j<n; j++) d->elem[j] = e[j];

  i = allocate_type_id(table);
  table->kind[i] = TUPLE_TYPE;
  table->desc[i].ptr = d;

  /*
   * set flags and card
   * - type_flags_conjunct sets all the bits correctky
   *   except possibly the exact card bit
   */
  flag = type_flags_conjunct(table, n, e);
  switch (flag) {
  case UNIT_TYPE_FLAGS: 
    // all components are unit types
    card = 1;
    break;

  case SMALL_TYPE_FLAGS:
    // all components are unit or small types
    card = type_card_product(table, n, e);
    if (card > UINT32_MAX) { 
      // the product does not fit in 32bits
      // change exact card to inexact card
      card = UINT32_MAX;
      flag = LARGE_TYPE_FLAGS;
    }
    break;

  default:
    assert(flag == LARGE_TYPE_FLAGS || 
	   (flag & CARD_FLAGS_MASK) == INFINITE_TYPE_FLAGS);
    card = UINT32_MAX;
    break;
  }

  assert(0 < card && card <= UINT32_MAX);
  table->card[i] = card;
  table->flags[i] = flag;

  return i;
}


/*
 * Add function type: (e[0], ..., e[n-1] --> r)
 */
static type_t new_function_type(type_table_t *table, uint32_t n, type_t *e, type_t r) {
  function_type_t *d;
  uint64_t card;
  type_t i;
  uint32_t j, flag, minmax;
  
  assert(0 < n && n <= YICES_MAX_ARITY);

  d = (function_type_t *) safe_malloc(sizeof(function_type_t) + n * sizeof(type_t));
  d->range = r;
  d->ndom = n;
  for (j=0; j<n; j++) d->domain[j] = e[j];

  i = allocate_type_id(table);
  table->kind[i] = FUNCTION_TYPE;
  table->desc[i].ptr = d;

  /*
   * Three of the function type's flags are inherited from the range:
   * - fun type is unit iff range is unit
   * - fun type is maximal iff range is maximal
   * - fun type is minimal iff range is minimal
   */
  flag = type_flags(table, r);
  minmax = flag & MINMAX_FLAGS_MASK; // save min and max bits

  /*
   * If the range is finite but not unit, then we check
   * whether all domains are finite.
   */
  if ((flag & (TYPE_IS_FINITE_MASK|TYPE_IS_UNIT_MASK)) == TYPE_IS_FINITE_MASK) {
    assert(flag == SMALL_TYPE_FLAGS || flag == LARGE_TYPE_FLAGS);
    flag &= type_flags_conjunct(table, n, e);
  }

  switch (flag) {
  case UNIT_TYPE_FLAGS:
    // singleton range so the function type is also a singleton
    card = 1;
    break;

  case SMALL_TYPE_FLAGS:
    // the range is small finite
    // all domains are small finite or unit
    card = fun_type_card(table, n, e, r);
    if (card > UINT32_MAX) {
      card = UINT32_MAX;
      flag = LARGE_TYPE_FLAGS;
    }
    break;

  default:
    // the range or at least one domain is infinite
    // or the range and all domains are finite but at least one 
    // of them is large.
    assert(flag == LARGE_TYPE_FLAGS || 
	   (flag & CARD_FLAGS_MASK) == INFINITE_TYPE_FLAGS);
    card = UINT32_MAX;
    break;
  }

  assert(0 < card && card <= UINT32_MAX);
  table->card[i] = card;
  table->flags[i] = minmax | (flag & CARD_FLAGS_MASK);

  return i;
}



/*
 * HASH CONSING
 */

/*
 * Objects for hash-consing
 */
typedef struct bv_type_hobj_s {
  int_hobj_t m;      // methods
  type_table_t *tbl;
  uint32_t size;
} bv_type_hobj_t;

typedef struct tuple_type_hobj_s {
  int_hobj_t m;
  type_table_t *tbl;
  uint32_t n;
  type_t *elem;
} tuple_type_hobj_t;

typedef struct function_type_hobj_s {
  int_hobj_t m;
  type_table_t *tbl;
  type_t range;
  uint32_t n;
  type_t *dom;
} function_type_hobj_t;


/*
 * Hash functions
 */
static uint32_t hash_bv_type(bv_type_hobj_t *p) {
  return jenkins_hash_pair(p->size, 0, 0x7838abe2);
}

static uint32_t hash_tuple_type(tuple_type_hobj_t *p) {
  return jenkins_hash_intarray_var(p->n, p->elem, 0x8193ea92);
}

static uint32_t hash_function_type(function_type_hobj_t *p) {
  uint32_t h;

  h = jenkins_hash_intarray_var(p->n, p->dom, 0x5ad7b72f);
  return jenkins_hash_pair(p->range, 0, h);
}


/*
 * Hash functions used during garbage collection.
 * Make sure they are consistent with the ones above.
 */
static uint32_t hash_bvtype(int32_t size) {
  return jenkins_hash_pair(size, 0, 0x7838abe2);  
}

static uint32_t hash_tupletype(tuple_type_t *p) {
  return jenkins_hash_intarray_var(p->nelem, p->elem, 0x8193ea92);
}

static uint32_t hash_funtype(function_type_t *p) {
  uint32_t h;
  h = jenkins_hash_intarray_var(p->ndom, p->domain, 0x5ad7b72f);
  return jenkins_hash_pair(p->range, 0, h);
}


/*
 * Comparison functions for hash consing
 */
static bool eq_bv_type(bv_type_hobj_t *p, type_t i) {
  type_table_t *table;

  table = p->tbl;
  return table->kind[i] == BITVECTOR_TYPE && table->desc[i].integer == p->size;
}

static bool eq_tuple_type(tuple_type_hobj_t *p, type_t i) {
  type_table_t *table;
  tuple_type_t *d;
  int32_t j;

  table = p->tbl;
  if (table->kind[i] != TUPLE_TYPE) return false;

  d = (tuple_type_t *) table->desc[i].ptr;
  if (d->nelem != p->n) return false;

  for (j=0; j<p->n; j++) {
    if (d->elem[j] != p->elem[j]) return false;
  }

  return true;
}

static bool eq_function_type(function_type_hobj_t *p, type_t i) {
  type_table_t *table;
  function_type_t *d;
  int32_t j;

  table = p->tbl;
  if (table->kind[i] != FUNCTION_TYPE) return false;

  d = (function_type_t *) table->desc[i].ptr;
  if (d->range != p->range || d->ndom != p->n) return false;

  for (j=0; j<p->n; j++) {
    if (d->domain[j] != p->dom[j]) return false;
  }

  return true;
}


/*
 * Builder functions
 */
static type_t build_bv_type(bv_type_hobj_t *p) {
  return new_bitvector_type(p->tbl, p->size);
}

static type_t build_tuple_type(tuple_type_hobj_t *p) {
  return new_tuple_type(p->tbl, p->n, p->elem);
}

static type_t build_function_type(function_type_hobj_t *p) {
  return new_function_type(p->tbl, p->n, p->dom, p->range);
}


/*
 * Global Hash Objects
 */
static bv_type_hobj_t bv_hobj = {
  { (hobj_hash_t) hash_bv_type, (hobj_eq_t) eq_bv_type, 
    (hobj_build_t) build_bv_type },
  NULL,
  0,
};

static tuple_type_hobj_t tuple_hobj = {
  { (hobj_hash_t) hash_tuple_type, (hobj_eq_t) eq_tuple_type, 
    (hobj_build_t) build_tuple_type },
  NULL,
  0,
  NULL,
};

static function_type_hobj_t function_hobj = {
  { (hobj_hash_t) hash_function_type, (hobj_eq_t) eq_function_type,
    (hobj_build_t) build_function_type },
  NULL,
  0,
  0,
  NULL,
};





/*
 * TABLE MANAGEMENT + EXPORTED TYPE CONSTRUCTORS
 *
 * NOTE: The constructors for uninterpreted and scalar types
 * are defined above. Thay don't use hash consing.
 */


/*
 * Initialize table: add the predefined types
 */
void init_type_table(type_table_t *table, uint32_t n) {
  type_table_init(table, n);
  add_primitive_types(table);
}

/*
 * Delete table: free all allocated memory
 */
void delete_type_table(type_table_t *table) {
  uint32_t i;

  // decrement refcount for all names
  for (i=0; i<table->nelems; i++) {
    if (table->name[i] != NULL) {
      string_decref(table->name[i]);
    }
  }

  // delete all allocated descriptors
  for (i=0; i<table->nelems; i++) {
    if (table->kind[i] == TUPLE_TYPE || table->kind[i] == FUNCTION_TYPE) {
      safe_free(table->desc[i].ptr);
    }
  }

  safe_free(table->kind);
  safe_free(table->desc);
  safe_free(table->card);
  safe_free(table->flags);
  safe_free(table->name);

  table->kind = NULL;
  table->desc = NULL;
  table->card = NULL;
  table->flags = NULL;
  table->name = NULL;

  delete_int_htbl(&table->htbl);
  delete_stbl(&table->stbl);

  if (table->sup_tbl != NULL) {
    delete_int_hmap2(table->sup_tbl);
    safe_free(table->sup_tbl);
    table->sup_tbl = NULL;
  }

  if (table->inf_tbl != NULL) {
    delete_int_hmap2(table->inf_tbl);
    safe_free(table->inf_tbl);
    table->inf_tbl = NULL;
  }
}


/*
 * Bitvector type
 */
type_t bv_type(type_table_t *table, uint32_t size) {
  assert(size > 0);
  bv_hobj.tbl = table;
  bv_hobj.size = size;
  return int_htbl_get_obj(&table->htbl, (int_hobj_t *) &bv_hobj);
}

/*
 * Tuple type
 */
type_t tuple_type(type_table_t *table, uint32_t n, type_t elem[]) {
  assert(0 < n && n <= YICES_MAX_ARITY);
  tuple_hobj.tbl = table;
  tuple_hobj.n = n;
  tuple_hobj.elem = elem;
  return int_htbl_get_obj(&table->htbl, (int_hobj_t *) &tuple_hobj);
}

/*
 * Function type
 */
type_t function_type(type_table_t *table, type_t range, uint32_t n, type_t dom[]) {
  assert(0 < n && n <= YICES_MAX_ARITY);
  function_hobj.tbl = table;
  function_hobj.range = range;
  function_hobj.n = n;
  function_hobj.dom = dom;
  return int_htbl_get_obj(&table->htbl, (int_hobj_t *) &function_hobj);  
}




/*
 * Assign name to type i.
 * - previous mapping of name to other types (if any) are hidden.
 * - name must have a reference counter attached to it (cf. clone_string
 *   in memalloc.h).
 */
void set_type_name(type_table_t *table, type_t i, char *name) {
  if (table->name[i] == NULL) {
    table->name[i] = name;
    string_incref(name);
  }
  stbl_add(&table->stbl, name, i);
  string_incref(name);
}

/*
 * Get type mapped to the name (or NULL_TYPE)
 */
type_t get_type_by_name(type_table_t *table, char *name) {
  // NULL_TYPE = -1 and stbl_find returns -1 if name is absent
  return stbl_find(&table->stbl, name);
}

/*
 * Remove a type name.
 */
void remove_type_name(type_table_t *table, char *name) {
  stbl_remove(&table->stbl, name);
}



/*
 * CARDINALITY
 */

/*
 * Approximate cardinality of tau[0] x ... x tau[n-1]
 * - returns the same value as card_of(tuple_type(tau[0] ... tau[n-1])) but does not
 *   construct the tuple type.
 */
uint32_t card_of_type_product(type_table_t *table, uint32_t n, type_t *tau) {
  uint64_t card;

  card = type_card_product(table, n, tau);
  if (card > UINT32_MAX) {
    card = UINT32_MAX;
  }
  assert(1 <= card && card <= UINT32_MAX);

  return (uint32_t) card;
}



/*
 * Approximate cardinality of the domain and range of a function type tau
 */
uint32_t card_of_domain_type(type_table_t *table, type_t tau) {
  function_type_t *d;

  d = function_type_desc(table, tau);
  return card_of_type_product(table, d->ndom, d->domain);
}

uint32_t card_of_range_type(type_table_t *table, type_t tau) {
  return type_card(table, function_type_range(table, tau));
}



/*
 * Check whether a function type has a finite domain or range
 * - tau must be a function type.
 */
bool type_has_finite_domain(type_table_t *table, type_t tau) {
  function_type_t *fun;
  uint32_t flag;

  fun = function_type_desc(table, tau);
  flag = type_flags_conjunct(table, fun->ndom, fun->domain);
  return flag & TYPE_IS_FINITE_MASK;
}

bool type_has_finite_range(type_table_t *table, type_t tau) {
  return is_finite_type(table, function_type_range(table, tau));
}






/*
 * COMMON SUPERTYPE
 */

/*
 * Try to compute sup(tau1, tau2) cheaply
 * - return UNKNOWN_TYPE if that fails
 */
#define UNKNOWN_TYPE (-2)

static type_t cheap_sup(type_table_t *table, type_t tau1, type_t tau2) {
  assert(good_type(table, tau1) && good_type(table, tau2));

  if (tau1 == tau2) {
    return tau1;
  }

  if ((tau1 == int_id && tau2 == real_id) || 
      (tau1 == real_id && tau2 == int_id)) {
    return real_id;
  }

  switch (table->kind[tau1]) {
  case TUPLE_TYPE:
    if (table->kind[tau2] != TUPLE_TYPE || 
	tuple_type_arity(table, tau1) != tuple_type_arity(table, tau2)) {
      return NULL_TYPE;
    }
    break;

  case FUNCTION_TYPE:
    if (table->kind[tau2] != FUNCTION_TYPE ||
	function_type_arity(table, tau1) != function_type_arity(table, tau2)) {
      return NULL_TYPE;
    }
    break;

  default:
    return NULL_TYPE;
  }

  return UNKNOWN_TYPE;
}



/*
 * Construct sup of two tuple types of equal arity n:
 * - first tuple components are a[0] .... a[n-1]
 * - second tuple components are b[0] ... b[n-1]
 * The result is either NULL_TYPE or (tuple s[0] ... s[n-1]) 
 * where s[i] = sup(a[i], b[i]).
 */
static type_t sup_tuple_types(type_table_t *table, uint32_t n, type_t *a, type_t *b) {
  type_t buffer[8];
  type_t *s;
  type_t aux;
  uint32_t i;

  /*
   * For intermediate results, we use a buffer of 8 types.
   * That should be enough in most cases. Otherwise
   * we allocate a larger buffer s.
   */
  s = buffer;
  if (n > 8) {
    s = (type_t *) safe_malloc(n * sizeof(type_t));    
  }

  for (i=0; i<n; i++) {
    aux = super_type(table, a[i], b[i]);
    if (aux == NULL_TYPE) goto done;
    s[i] = aux;
  }
  aux = tuple_type(table, n, s);

 done:
  if (n > 8) {
    safe_free(s);
  }
  return aux;
}


/*
 * Check whether a[0 ... n-1] and b[0 ... n-1]
 * are equal (i.e., same function domain).
 */
static bool equal_type_arrays(uint32_t n, type_t *a, type_t *b) {
  uint32_t i;

  for (i=0; i<n; i++) {
    if (a[i] != b[i]) return false;
  }
  return true;
}


/*
 * Construct sup of two function types sigma1 and sigma2 of 
 * equal domain and arity.
 * - n = arity 
 * - a[0] ... a[n-1] = domain type
 * - tau1 = range of sigma1
 * - tau2 = range of sigma2
 *
 * The result is either the function type [a[0] ... a[n-1] --> sup(tau1, tau2)]
 * or NULL_TYPE.
 */
static type_t sup_fun_types(type_table_t *table, uint32_t n, type_t *a, type_t tau1, type_t tau2) {
  type_t aux;

  aux = super_type(table, tau1, tau2);
  if (aux != NULL_TYPE) {
    aux = function_type(table, aux, n, a);
  }
  return aux;
}


/*
 * Compute the smallest supertype of tau1 and tau2.  Use the cheap
 * method first. If that fails, compute the result and keep the result
 * in the internal sup_tbl cache.
 */
type_t super_type(type_table_t *table, type_t tau1, type_t tau2) {  
  tuple_type_t *tup1, *tup2;
  function_type_t *fun1, *fun2;
  int_hmap2_t *sup_tbl;
  int_hmap2_rec_t *r;
  type_t aux;

  assert(good_type(table, tau1) && good_type(table, tau2));

  aux = cheap_sup(table, tau1, tau2);
  if (aux == UNKNOWN_TYPE) {
    /*
     * Cheap_sup failed.
     * Check whether sup(tau1, tau2) is already in the cache.
     * If it's not do the computation and add the 
     * result to the cache.
     */

    // Normalize. We want tau1 < tau2
    if (tau1 > tau2) {
      aux = tau1; tau1 = tau2; tau2 = aux;
    }
    assert(tau1 < tau2);
  
    sup_tbl = get_sup_table(table);
    r = int_hmap2_find(sup_tbl, tau1, tau2);
    if (r != NULL) {
      aux = r->val;
    } else {
      /*
       * The result is not in the cache.
       */
      if (table->kind[tau1] == TUPLE_TYPE) {
	tup1 = tuple_type_desc(table, tau1);
	tup2 = tuple_type_desc(table, tau2);
	assert(tup1->nelem == tup2->nelem);
	aux = sup_tuple_types(table, tup1->nelem, tup1->elem, tup2->elem);

      } else {
	fun1 = function_type_desc(table, tau1);
	fun2 = function_type_desc(table, tau2);
	assert(fun1->ndom == fun2->ndom);
	aux = NULL_TYPE;
	if (equal_type_arrays(fun1->ndom, fun1->domain, fun2->domain)) {
	  aux = sup_fun_types(table, fun1->ndom, fun1->domain, fun1->range, fun2->range);
	}
      }

      int_hmap2_add(sup_tbl, tau1, tau2, aux);
    }  
  }

  assert(aux == NULL_TYPE || good_type(table, aux));

  return aux;
}



/*
 * COMMON SUBTYPE
 */

/*
 * Try to compute inf(tau1, tau2) cheaply.
 * Return UNKNOWN_TYPE if that fails.
 */
static type_t cheap_inf(type_table_t *table, type_t tau1, type_t tau2) {
  assert(good_type(table, tau1) && good_type(table, tau2));

  if (tau1 == tau2) {
    return tau1;
  }

  if ((tau1 == int_id && tau2 == real_id) || 
      (tau1 == real_id && tau2 == int_id)) {
    return int_id;
  }

  switch (table->kind[tau1]) {
  case TUPLE_TYPE:
    if (table->kind[tau2] != TUPLE_TYPE || 
	tuple_type_arity(table, tau1) != tuple_type_arity(table, tau2)) {
      return NULL_TYPE;
    }
    break;

  case FUNCTION_TYPE:
    if (table->kind[tau2] != FUNCTION_TYPE ||
	function_type_arity(table, tau1) != function_type_arity(table, tau2)) {
      return NULL_TYPE;
    }
    break;

  default:
    return NULL_TYPE;
  }

  return UNKNOWN_TYPE;
}



/*
 * Construct inf of two tuple types of equal arity n:
 * - first tuple components are a[0] .... a[n-1]
 * - second tuple components are b[0] ... b[n-1]
 * The result is either NULL_TYPE or (tuple s[0] ... s[n-1]) 
 * where s[i] = inf(a[i], b[i]).
 */
static type_t inf_tuple_types(type_table_t *table, uint32_t n, type_t *a, type_t *b) {
  type_t buffer[8];
  type_t *s;
  type_t aux;
  uint32_t i;

  /*
   * For intermediate results, we use a buffer of 8 types.
   * That should be enough in most cases. Otherwise
   * we allocate a larger buffer s.
   */
  s = buffer;
  if (n > 8) {
    s = (type_t *) safe_malloc(n * sizeof(type_t));    
  }

  for (i=0; i<n; i++) {
    aux = inf_type(table, a[i], b[i]);
    if (aux == NULL_TYPE) goto done;
    s[i] = aux;
  }
  aux = tuple_type(table, n, s);

 done:
  if (n > 8) {
    safe_free(s);
  }
  return aux;
}


/*
 * Construct inf of two function types sigma1 and sigma2 of 
 * equal domain and arity.
 * - n = arity 
 * - a[0] ... a[n-1] = domain type
 * - tau1 = range of sigma1
 * - tau2 = range of sigma2
 *
 * The result is either the function type [a[0] ... a[n-1] --> inf(tau1, tau2)]
 * or NULL_TYPE.
 */
static type_t inf_fun_types(type_table_t *table, uint32_t n, type_t *a, type_t tau1, type_t tau2) {
  type_t aux;

  aux = inf_type(table, tau1, tau2);
  if (aux != NULL_TYPE) {
    aux = function_type(table, aux, n, a);
  }
  return aux;
}


/*
 * Compute the largest common subtype of tau1 and tau2.  Use the cheap
 * method first. If that fails, compute the result and keep the result
 * in the internal inf_tbl cache.
 */
type_t inf_type(type_table_t *table, type_t tau1, type_t tau2) {  
  tuple_type_t *tup1, *tup2;
  function_type_t *fun1, *fun2;
  int_hmap2_t *inf_tbl;
  int_hmap2_rec_t *r;
  type_t aux;

  assert(good_type(table, tau1) && good_type(table, tau2));

  aux = cheap_inf(table, tau1, tau2);
  if (aux == UNKNOWN_TYPE) {
    /*
     * Cheap_inf failed.
     * Check whether inf(tau1, tau2) is already in the cache.
     * If it's not do the computation and add the 
     * result to the cache.
     */

    // Normalize. We want tau1 < tau2
    if (tau1 > tau2) {
      aux = tau1; tau1 = tau2; tau2 = aux;
    }
    assert(tau1 < tau2);
  
    inf_tbl = get_inf_table(table);
    r = int_hmap2_find(inf_tbl, tau1, tau2);
    if (r != NULL) {
      aux = r->val;
    } else {
      /*
       * The result is not in the cache.
       */
      if (table->kind[tau1] == TUPLE_TYPE) {
	tup1 = tuple_type_desc(table, tau1);
	tup2 = tuple_type_desc(table, tau2);
	assert(tup1->nelem == tup2->nelem);
	aux = inf_tuple_types(table, tup1->nelem, tup1->elem, tup2->elem);

      } else {
	fun1 = function_type_desc(table, tau1);
	fun2 = function_type_desc(table, tau2);
	assert(fun1->ndom == fun2->ndom);
	aux = NULL_TYPE;
	if (equal_type_arrays(fun1->ndom, fun1->domain, fun2->domain)) {
	  aux = inf_fun_types(table, fun1->ndom, fun1->domain, fun1->range, fun2->range);
	}
      }

      int_hmap2_add(inf_tbl, tau1, tau2, aux);
    }  
  }

  assert(aux == NULL_TYPE || good_type(table, aux));

  return aux;
}




/*
 * SUBTYPE AND COMPATIBILITY
 */

/*
 * Check whether tau1 is a subtype if tau2.
 *
 * Side effects: this is implemented using super_type so this may create
 * new types in the table.
 */
bool is_subtype(type_table_t *table, type_t tau1, type_t tau2) {
  return super_type(table, tau1, tau2) == tau2;
}


/*
 * Check whether tau1 and tau2 are compatible.
 *
 * Side effects: use the super_type function. So this may create new 
 * types in the table.
 */
bool compatible_types(type_table_t *table, type_t tau1, type_t tau2) {
  return super_type(table, tau1, tau2) != NULL_TYPE;
}



/*
 * GARBAGE COLLECTION
 */

/*
 * Remove type i from the hash-consing table
 */
static void erase_hcons_type(type_table_t *table, type_t i) {
  uint32_t k;

  switch (table->kind[i]) {
  case BITVECTOR_TYPE:
    k = hash_bvtype(table->desc[i].integer);
    break;

  case TUPLE_TYPE:
    k = hash_tupletype(table->desc[i].ptr);
    break;

  case FUNCTION_TYPE:
    k = hash_funtype(table->desc[i].ptr);
    break;

  default: 
    return;
  }

  int_htbl_erase_record(&table->htbl, k, i);
}




/*
 * Mark all descendants of i whose id is less than ptr.
 * - i must be a marked type (and not already deleted)
 *
 * NOTE: we use a recursive function to propagate the marks.
 * That should be safe as there's little risk of stack overflow.
 */
static void mark_reachable_types(type_table_t *table, type_t ptr, type_t i);

// mark i if it's not marked already then explore its children if i < ptr
static void mark_and_explore(type_table_t *table, type_t ptr, type_t i) {
  if (! type_is_marked(table, i)) {
    type_table_set_gc_mark(table, i);
    if (i < ptr) {
      mark_reachable_types(table, ptr, i);
    }
  }
}

static void mark_reachable_types(type_table_t *table, type_t ptr, type_t i) {
  tuple_type_t *tup;
  function_type_t *fun;
  uint32_t n, j;

  assert(type_is_marked(table, i) &&  table->kind[i] != UNUSED_TYPE);

  switch (table->kind[i]) {
  case TUPLE_TYPE:
    tup = table->desc[i].ptr;
    n = tup->nelem;
    for (j=0; j<n; j++) {
      mark_and_explore(table, ptr, tup->elem[j]);
    }
    break;

  case FUNCTION_TYPE:
    fun = table->desc[i].ptr;
    mark_and_explore(table, ptr, fun->range);
    n = fun->ndom;
    for (j=0; j<n; j++) {
      mark_and_explore(table, ptr, fun->domain[j]);
    }
    break;

  default:
    break;
  }
}


/*
 * Propagate the marks:
 * - on entry: all roots are marked
 * - on exit: every type reachable from a root is marked
 */
static void mark_live_types(type_table_t *table) {
  uint32_t i, n;

  n = table->nelems;
  for (i=0; i<n; i++) {
    if (type_is_marked(table, i)) {
      mark_reachable_types(table, i, i);
    }
  }
}


/*
 * Iterator to mark types present in the symbol table
 * - aux must be a pointer to the type table
 * - r = live record in the symbol table so r->value
 *   is the id of a type to preserve.
 */
static void mark_symbol(void *aux, stbl_rec_t *r) {
  type_table_set_gc_mark(aux, r->value);
}


/*
 * Keep-alive function for the sup/inf caches
 * - record (k0, k1 --> x) is kept in the caches 
 *   if k0, k1, and x haven't been deleted
 * - aux is a pointer to the type table
 */
static bool keep_in_cache(void *aux, int_hmap2_rec_t *r) {
  return good_type(aux, r->k0) && good_type(aux, r->k1) && 
    good_type(aux, r->val);
}

/*
 * Call the garbage collector:
 * - delete every type not reachable from a root
 * - cleanup the caches
 * - then clear all the marks
 */
void type_table_gc(type_table_t *table)  {
  uint32_t i, n;

  // mark every type present in the symbol table
  stbl_iterate(&table->stbl, table, mark_symbol);

  // mark the three predefined types
  type_table_set_gc_mark(table, bool_id);
  type_table_set_gc_mark(table, int_id);
  type_table_set_gc_mark(table, real_id);

  // propagate the marks
  mark_live_types(table);

  // delete every unmarked type
  n = table->nelems;
  for (i=0; i<n; i++) {
    if (! type_is_marked(table, i)) {
      erase_hcons_type(table, i);
      erase_type(table, i);
    }
    type_table_clr_gc_mark(table, i);
  }

  // cleanup the inf/sup caches if they exist
  if (table->sup_tbl != NULL) {
    int_hmap2_gc(table->sup_tbl, table, keep_in_cache);
  }

  if (table->inf_tbl != NULL) {
    int_hmap2_gc(table->inf_tbl, table, keep_in_cache);
  }
}
