#include <stddef.h>
#include "compiler.h"

static void unexarg(void)
{
	error("unexpected argument");
}

static void missedarg(unsigned narg, unsigned ti)
{
	/* Make sure no arguments is acceptable */
	if (!(narg == 0 || ti == ELLIPSIS || ti == VOID))
		error("missing argument");
}

/*
 *	At the moment this is used for functions, but it will be used for
 *	casting, hence all the sanity checks.
 */
struct node *typeconv(struct node *n, unsigned type, unsigned warn)
{
	unsigned nt = type_canonical(n->type);
	if (!PTR(nt)) {
		/* You can cast pointers to things but not actual block
		   classes */
		if (!IS_SIMPLE(nt) || !IS_ARITH(nt) ||
			!IS_SIMPLE(type) || !IS_ARITH(type)) {
			error("invalid type conversion");
			return n;
		}
	} else {
		if (type_pointerconv(n, type))
			return make_cast(n, type);
	}
	if (nt == type || (IS_ARITH(nt) && IS_ARITH(type)))
		return make_cast(n, type);
	if ((IS_ARITH(nt) && PTR(type)) || (IS_ARITH(type) && PTR(nt))) {
		if (!warn)
			return make_cast(n, type);
	}
	typemismatch();
	n->type = nt;
	return n;
}

/*
 *	Perform the implicit legacy type conversions C specifies for
 *	unprototyped arguments
 */
struct node *typeconv_implicit(struct node *n)
{
	unsigned t = n->type;
	if (t == CCHAR || t == UCHAR)
		return typeconv(n, CINT, 0);
	if (t == FLOAT)
		return typeconv(n, DOUBLE, 0);
	n->type = type_canonical(t);
	return n;
}

/*
 *	Build an argument tree for right to left stacking
 *
 *	TODO: both here and in the space allocation we need to
 *	do type / size fixes for argument spacing. For example on an 8080
 *	we always push 2 bytes so char as arg takes 2 and we need to do
 *	the right thing.
 */
struct node *call_args(unsigned *narg, unsigned *argt, unsigned *argsize)
{
	struct node *n = expression_tree(0);

	/* See what argument type handling is needed */
	if (*argt == VOID)
		unexarg();
	/* Implicit */
	else if (*argt == ELLIPSIS)
		n = typeconv_implicit(n);
	else {
		/* Explicit prototyped argument */
		if (*narg) {
			n = typeconv(n, *argt++, 1);
			(*narg)--;
		} else
			unexarg();
	}
	*argsize += target_argsize(n->type);
	if (match(T_COMMA))
		/* Switch around for calling order */
		return tree(T_COMMA, call_args(narg, argt, argsize), n);
	require(T_RPAREN);
	return n;
}

/*
 *	Generate a function call tree - no type checking arg counts etc
 *	yet. Take any arguments for a function we've not seen a prototype for.
 */

static unsigned dummy_argp = ELLIPSIS;

struct node *function_call(struct node *n)
{
	unsigned type;
	unsigned *argt, *argp;
	unsigned argsize = 0;
	unsigned narg;

	if (!IS_FUNCTION(n->type)) {
		error("not a function");
		return n;
	}
	type = func_return(n->type);
	argt = func_args(n->type);

	if (!argt)
		fatal("narg");
	narg = *argt;

	if (narg == 0)
		argp = &dummy_argp;
	else
		argp = argt + 1;

	/* A function without arguments */
	if (match(T_RPAREN)) {
		/* Make sure no arguments is acceptable */
		n  = tree(T_FUNCCALL, NULL, n);
		missedarg(narg, argp[0]);
	} else {
		n = tree(T_FUNCCALL, call_args(&narg, argp, &argsize), n);
		missedarg(narg, argp[0]);
	}
	/* Always emit this - some targets have other uses for knowing
	   the boundary of a function call return */
	n->type = type;
	n = tree(T_CLEANUP, n, make_constant(argsize, UINT));
	return n;
}

/*
 *	Postfixed array and structure dereferences (basically the same but
 *	one is named and re-typed), and function calls.
 */
static struct node *hier11(void)
{
	int direct;
	struct node *l, *r;
	unsigned ptr;
	unsigned scale;
	unsigned *tag;
	unsigned lt;

	l = primary();
	lt = l->type;;

	ptr = PTR(lt) || IS_ARRAY(lt);
	if (token == T_LSQUARE || token == T_LPAREN || token == T_DOT
	    || token == T_POINTSTO) {
		for (;;) {
			if (match(T_LSQUARE)) {
				if (ptr == 0) {
					error("can't subscript");
					junk();
					require(T_RSQUARE);
					return (0);
				}
				r = expression_tree(1);
				require(T_RSQUARE);
				/* Need a proper method for this stuff */
				/* TODO arrays */
				scale = type_ptrscale(lt);
				l = tree(T_PLUS, l,
					 tree(T_STAR, r,
					      make_constant(scale, UINT)));
				l->flags |= LVAL;
				/* Force the type back correct as tree()
				   defaults to the RH type */
				l->type = lt;
			} else if (match(T_LPAREN)) {
				l = function_call(l);
			} else if ((direct = match(T_DOT))
				   || match(T_POINTSTO)) {
				if (direct == 0)
					l = tree(T_DEREF, NULL, l);
				if (PTR(l->type)
				    || !IS_STRUCT(l->type)) {
					error("can't take member");
					junk();
					return 0;
				}
				tag = struct_find_member(l->type, symname());
				if (tag == NULL) {
					error("unknown member");
					/* So we don't internal error later */
					l->type = CINT;
					return l;
				}
				l = tree(T_PLUS, l,
					 make_constant(tag[2], UINT));
				l->flags |= LVAL;
				l->type = tag[1];
			} else
				return l;
		}
	}
	return l;
}

/*
 *	Unary operators
 *
 *	type_scale() typechecks the increment/decrement operators
 */
static struct node *hier10(void)
{
	struct node *l, *r;
	unsigned op;
	unsigned name;
	unsigned t;

	op = token;
	if (token != T_PLUSPLUS
	    && token != T_MINUSMINUS
	    && token != T_MINUS
	    && token != T_TILDE
	    && token != T_LPAREN
	    && token != T_BANG && token != T_STAR && token != T_AND) {
		/* Check for trailing forms */
		l = hier11();
		if (token == T_PLUSPLUS || token == T_MINUSMINUS) {
			if (!(l->flags & LVAL)) {
				needlval();
				return l;
			}
			op = token;
			/* It's an lval so we want the pointer form */
			unsigned s = type_scale(l->type);
			next_token();
			/* Put the constant on the right for convenience */
			r = tree(op, l, make_constant(s, UINT));
			/* Fix up the type */
			r->type = l->type;
			return r;
		}
		return l;
	}

	next_token();
	switch (op) {
	case T_PLUSPLUS:
	case T_MINUSMINUS:
		r = hier10();
		if (!(r->flags & LVAL)) {
			needlval();
			return r;
		}
		if (op == T_PLUSPLUS)
			op = T_PLUSEQ;
		else
			op = T_MINUSEQ;
		return tree(op, r, make_constant(type_scale(r->type), UINT));
	case T_TILDE:
		/* Floating point bit ops are not allowed */
		r = make_rval(hier10());
		if (!IS_INTARITH(r->type))
			badtype();
		return tree(op, NULL, r);
	case T_MINUS:
		/* Disambiguate */
		op = T_NEGATE;
	case T_BANG:
		/* Floating point allowed */
		r = make_rval(hier10());
		if (!IS_ARITH(r->type))
			badtype();
		return tree(op, NULL, r);
	case T_STAR:
		r = make_rval(hier10());
		/* TODO: To review - array */
		if (!PTR(type_canonical(r->type)))
			badtype();
		r = tree(T_DEREF, NULL, r);
		r->type = r->right->type - 1;
		return r;
	case T_AND:
		r = hier10();
		/* If it's an lvalue then just stop being an lvalue */
		if (r->flags & LVAL) {
			r->flags &= ~LVAL;
			/* We are now a pointer to */
			r->type = type_ptr(r->type);
			return r;
		}
		r = tree(T_ADDROF, NULL, r);
		r->type = type_addrof(r->type);
		return r;
	case T_LPAREN:
		/* Should be a type without a name */
		t = type_and_name(S_AUTO, &name, 0, UNKNOWN);
		require(T_RPAREN);
		if (t == UNKNOWN || name)
			badtype();
		return typeconv(hier10(), t, 0);
	}
	fatal("h10");
}

/*
 *	Multiplication, division and remainder
 *	The '%' operator does not apply to floating point.
 */
static struct node *hier9(void)
{
	struct node *l;
	struct node *r;
	unsigned op;
	l = hier10();
	if (token != T_STAR && token != T_PERCENT && token != T_SLASH)
		return l;
	l = make_rval(l);
	op = token;
	next_token();
	r = make_rval(hier9());
	if (op == T_PERCENT)
		return intarith_tree(op, l, r);
	else
		return arith_tree(op, l, r);
}

/*
 *	Addition and subtraction. Messy because of the pointer scaling
 *	rules.
 */
static struct node *hier8(void)
{
	struct node *l, *r;
	unsigned op;
	unsigned scale = 1;
	unsigned scalediv;
	l = hier9();
	if (token != T_PLUS && token != T_MINUS)
		return l;
	op = token;
	next_token();
	l = make_rval(l);
	r = make_rval(hier8());
	/* This can currently do silly things but the typechecks when added
	   will stop them from mattering */
	if (op == T_PLUS) {
		/* if left is pointer and right is int, scale right */
		scale =
		    type_ptrscale_binop(op, l, r, &scalediv);
	} else if (op == T_MINUS) {
		/* if dbl, can only be: pointer - int, or
		   pointer - pointer, thus,
		   in first case, int is scaled up,
		   in second, result is scaled down. */
		scale =
		    type_ptrscale_binop(op, l, r, &scalediv);
	}
	/* The type checking was done in type_ptrscale_binop */
	if (scale == 1)
		return tree(op, l, r);
	if (scalediv)
		return tree(T_SLASH, tree(op, l, r), make_constant(scale, UINT));
	if (PTR(l->type))
		return tree(op, l, tree(T_STAR, r, make_constant(scale, UINT)));
	else
		return tree(op, tree(T_STAR, l, make_constant(scale, UINT)), r);
}


/*
 *	Shifts
 */
static struct node *hier7(void)
{
	struct node *l;
	unsigned op;
	l = hier8();
	if (token != T_GTGT && token != T_LTLT)
		return l;
	op = token;
	next_token();
	/* The tree code knows about the shift rule being different for types */
	return intarith_tree(op, make_rval(l), make_rval(hier7()));
}

/*
 *	Ordered comparison operators
 */
static struct node *hier6(void)
{
	struct node *l;
	unsigned op;
	l = hier7();
	if (token != T_LT && token != T_GT
	    && token != T_LTEQ && token != T_GTEQ)
		return (l);
	op = token;
	next_token();
	return ordercomp_tree(op, make_rval(l), make_rval(hier6()));
}

/*
 *	Equality and not equal operators
 */
static struct node *hier5(void)
{
	struct node *l;
	unsigned op;
	l = hier6();
	if (token != T_EQEQ && token != T_BANGEQ)
		return (l);
	op = token;
	next_token();
	return ordercomp_tree(op, make_rval(l), make_rval(hier5()));
}

/*
 *	Bitwise and
 */
static struct node *hier4(void)
{
	struct node *l;
	l = hier5();
	if (!match(T_AND))
		return (l);
	return intarith_tree(T_AND, make_rval(l), make_rval(hier4()));
}

/*
 *	Bitwise xor
 */
static struct node *hier3(void)
{
	struct node *l;
	l = hier4();
	if (!match(T_HAT))
		return (l);
	return intarith_tree(T_HAT, make_rval(l), make_rval(hier3()));
}

/*
 *	Bitwise or
 */
static struct node *hier2(void)
{
	struct node *l;
	l = hier3();
	if (!match(T_OR))
		return (l);
	return intarith_tree(T_OR, make_rval(l), make_rval(hier2()));
}

/**
 * processes logical and &&
 * @param lval
 * @return 0 or 1, fetch or no fetch
 */
static struct node *hier1c(void)
{
	struct node *l;
	l = hier2();
	if (!match(T_ANDAND))
		return (l);
	return logic_tree(T_ANDAND, make_rval(l), make_rval(hier1c()));
}

/**
 * processes logical or ||
 * @param lval
 * @return 0 or 1, fetch or no fetch
 */
static struct node *hier1b(void)
{
	struct node *l;
	l = hier1c();
	if (!match(T_OROR))
		return (l);
	return logic_tree(T_OROR, make_rval(l), make_rval(hier1b()));
}

/*
 *	The ?: operator. We turn this into trees, the backend turns it into
 *	bramches/
 *
 *	Type rules are bool for ? and both sides matching for :
 */
static struct node *hier1a(void)
{
	struct node *l;
	struct node *a1, *a2;
	unsigned lt;

	l = hier1b();
	if (!match(T_QUESTION))
		return (l);
	l = make_rval(l);
	lt = l->type;

	/* Must be convertible to a boolean != 0 test */
	/* TODO: is float ? valid */
	if (!PTR(lt) && !IS_ARITH(lt))
		badtype();
	/* Now do the left of the colon */
	a1 = make_rval(hier1a());
	if (!match(T_COLON)) {
		error("missing colon");
		return l;
	}
	a2 = make_rval(hier1b());
	/* Check the two sides of colon are compatible */
	if (!(type_pointermatch(a1, a2) || (IS_ARITH(a1->type) && IS_ARITH(a2->type))))
		badtype();
	return tree(T_QUESTION, tree(T_BOOL, NULL, l), tree(T_COLON, a1, typeconv(a2, a1->type, 1)));
}


/*
 *	Assignment between an lval on the left and an rval on the right
 *
 *	Handle pointer scaling on += and -= by emitting the maths into the
 *	tree.
 */
struct node *hier1(void)
{
	struct node *l, *r;
	unsigned fc;
	unsigned scale = 1;
	l = hier1a();
	if (match(T_EQ)) {
		if ((l->flags & LVAL) == 0)
			needlval();
		r = make_rval(hier1());
		return assign_tree(l, r);	/* Assignment */
	} else {
		fc = token;
		if (match(T_MINUSEQ) ||
		    match(T_PLUSEQ) ||
		    match(T_STAREQ) ||
		    match(T_SLASHEQ) ||
		    match(T_PERCENTEQ) ||
		    match(T_SHREQ) ||
		    match(T_SHLEQ) ||
		    match(T_ANDEQ) || match(T_HATEQ) || match(T_OREQ)) {
			if ((l->flags & LVAL) == 0) {
				needlval();
				return l;
			}
			r = make_rval(hier1());
			switch (fc) {
			case T_MINUSEQ:
			case T_PLUSEQ:
				scale = type_scale(l->type);
				break;
			}
			if (scale)
				return tree(fc, l,
					    tree(T_STAR, r,
						 make_constant(scale, UINT)));
			return tree(fc, l, r);
		} else
			return l;
	}
	/* gcc */
	return NULL;
}

/*
 *	Top level of the expression tree. Make the tree an rval in case
 *	we need the result. Allow for both the expr,expr,expr format and
 *	the cases where C doesnt allow it (expr, expr in function calls
 *	or initializers is not the same
 */
struct node *expression_tree(unsigned comma)
{
	struct node *n;
	/* Build a tree of comma operations */
	n = make_rval(hier1());
	if (!comma || !match(T_COMMA))
		return n;
	n = tree(T_COMMA, n, expression_tree(comma));
	return n;
}


/* Generate an expression and write it the output */
unsigned expression(unsigned comma, unsigned mkbool, unsigned noret)
{
	struct node *n;
	unsigned t;
	if (token == T_SEMICOLON)
		return VOID;
	n = expression_tree(comma);
	/* FIXME: type check for the boolify */
	if (mkbool)
		n = tree(T_BOOL, NULL, n);
	if (noret)
		n->flags |= NORETURN;
	t = n->type;
	write_tree(n);
	return t;
}

/* We need a another version of this for initializers that allows global or
   static names (and string labels) too */

unsigned const_int_expression(void)
{
	unsigned v = 1;
	struct node *n = expression_tree(0);

	if (n->op == T_CONSTANT)
		v = n->value;
	else
		error("not constant");
	free_tree(n);
	return v;
}

unsigned bracketed_expression(unsigned mkbool)
{
	unsigned t;
	require(T_LPAREN);
	t = expression(1, mkbool, 0);
	require(T_RPAREN);
	return t;
}

void expression_or_null(unsigned mkbool, unsigned noret)
{
	if (token == T_SEMICOLON || token == T_RPAREN) {
		write_tree(tree(T_NULL, NULL, NULL));
		/* null */
	} else
		expression(1, mkbool, noret);
}
