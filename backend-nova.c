#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "compiler.h"
#include "backend.h"

#define BYTE(x)		(((unsigned)(x)) & 0xFF)
#define WORD(x)		(((unsigned)(x)) & 0xFFFF)

/*
 *	State for the current function
 */
static unsigned frame_len;	/* Number of bytes of stack frame */
static unsigned sp;		/* Stack pointer offset tracking */
static unsigned argbase;	/* Argument offset */
static unsigned unreachable;	/* Is code currently unreachable */

#define ARGBASE	5		/* 5 words */

/* Chance to rewrite the tree from the top rather than none by node
   upwards. We will use this for 8bit ops at some point and for cconly
   propagation */
struct node *gen_rewrite(struct node *n)
{
	return n;
}

/*
 *	Byte sizes although we are a word machine
 */
static unsigned get_size(unsigned t)
{
	if (PTR(t))
		return 2;
	if (t == CSHORT || t == USHORT)
		return 2;
	if (t == CCHAR || t == UCHAR)
		return 1;
	if (t == CLONG || t == ULONG || t == FLOAT)
		return 4;
	if (t == CLONGLONG || t == ULONGLONG || t == DOUBLE)
		return 8;
	if (t == VOID)
		return 0;
	error("gs");
	return 0;
}

static unsigned get_stack_size(unsigned t)
{
	unsigned n = get_size(t);
	if (n == 1)
		return 2;
	return n;
}

#define T_NREF		(T_USER)		/* Load of C global/static */
#define T_CALLNAME	(T_USER+1)		/* Function call by name */
#define T_NSTORE	(T_USER+2)		/* Store to a C global/static */
#define T_LREF		(T_USER+3)		/* Ditto for local */
#define T_LSTORE	(T_USER+4)
#define T_LBREF		(T_USER+5)		/* Ditto for labelled strings or local static */
#define T_LBSTORE	(T_USER+6)
#define T_RREF		(T_USER+7)
#define T_RSTORE	(T_USER+8)
#define T_RDEREF	(T_USER+9)		/* *regptr */
#define T_REQ		(T_USER+10)		/* *regptr */

static void squash_node(struct node *n, struct node *o)
{
	n->value = o->value;
	n->val2 = o->val2;
	n->snum = o->snum;
	free_node(o);
}

static void squash_left(struct node *n, unsigned op)
{
	struct node *l = n->left;
	n->op = op;
	squash_node(n, l);
	n->left = NULL;
}

static void squash_right(struct node *n, unsigned op)
{
	struct node *r = n->right;
	n->op = op;
	squash_node(n, r);
	n->right = NULL;
}

/*
 *	Our chance to do tree rewriting. We don't do much
 *	at this point, but we do rewrite name references and function calls
 *	to make them easier to process.
 */
struct node *gen_rewrite_node(struct node *n)
{
	struct node *l = n->left;
	struct node *r = n->right;
	unsigned op = n->op;
	unsigned nt = n->type;

	/* TODO: implement derefplus as we can fold small values into
	   a deref pattern */

	/* Rewrite references into a load operation. Don't do this with
	   char or long. We can probably do char sanely later but char
	   is painful */
	if (nt == CSHORT || nt == USHORT || PTR(nt)) {
		if (op == T_DEREF) {
			if (r->op == T_LOCAL || r->op == T_ARGUMENT) {
				if (r->op == T_ARGUMENT)
					r->value -= argbase + frame_len;
				squash_right(n, T_LREF);
				return n;
			}
			if (r->op == T_REG) {
				squash_right(n, T_RREF);
				return n;
			}
			if (r->op == T_NAME) {
				squash_right(n, T_NREF);
				return n;
			}
			if (r->op == T_LABEL) {
				squash_right(n, T_LBREF);
				return n;
			}
		}
		if (op == T_EQ) {
			if (l->op == T_NAME) {
				squash_left(n, T_NSTORE);
				return n;
			}
			if (l->op == T_LABEL) {
				squash_left(n, T_LBSTORE);
				return n;
			}
			if (l->op == T_LOCAL || l->op == T_ARGUMENT) {
				if (l->op == T_ARGUMENT)
					l->value += argbase + frame_len;
				squash_left(n, T_LSTORE);
				return n;
			}
			if (l->op == T_REG) {
				squash_left(n, T_RSTORE);
				return n;
			}
		}
	}
	/* Eliminate casts for sign, pointer conversion or same */
	if (op == T_CAST) {
		if (nt == r->type || (nt ^ r->type) == UNSIGNED ||
		 (PTR(nt) && PTR(r->type))) {
			free_node(n);
			return r;
		}
	}
	/* Rewrite function call of a name into a new node so we can
	   turn it easily into call xyz */
	if (op == T_FUNCCALL && r->op == T_NAME && PTR(r->type) == 1) {
		n->op = T_CALLNAME;
		n->snum = r->snum;
		n->value = r->value;
		free_node(r);
		n->right = NULL;
	}
	return n;
}

/* Export the C symbol */
void gen_export(const char *name)
{
	printf("	.export _%s\n", name);
}

void gen_segment(unsigned s)
{
	switch(s) {
	case A_CODE:
		printf("\t.code\n");
		break;
	case A_DATA:
		printf("\t.data\n");
		break;
	case A_LITERAL:
		printf("\t.literal\n");
		break;
	case A_BSS:
		printf("\t.bss\n");
		break;
	default:
		error("gseg");
	}
}

void gen_prologue(const char *name)
{
	unreachable = 0;
	printf("_%s:\n", name);
}

static void repeated_op(unsigned n, char *op)
{
	while(n--)
		printf("\t%s\n", op);
}
	
/* Generate the stack frame */
void gen_frame(unsigned size, unsigned aframe)
{
	frame_len = size;
	argbase = ARGBASE;
	sp = 0;
	/* Remember the stack grows upwards so values are negative offsets */
	printf("\tsav\n");
	printf("\tisz -4,3\n");	/* Will never skip */
	if (size == 0)
		return;
	if (size >= 5) {
		printf("\tmfsp 1\n");
		printf("\tlda 0,2,1\n");
		printf("\tadd 0,1,skip\n");
		printf("\t.word %u\n", size / 2);
		printf("\tmtsp 1\n");
	} else
		repeated_op(size / 2, "push 0");
	printf(";\n");
}

/* The return restores all the registers so we have to patch the stack frame */
void gen_epilogue(unsigned size, unsigned argsize)
{
	if (sp != 0)
		error("sp");
	if (unreachable)
		return;
	if (size >= 5) {
		printf("\tmfsp 1\n");
		printf("\tlda 0,2,1\n");
		printf("\tadd 0,1,skip\n");
		printf("\t.word %u\n", ((-size / 2) & 0xFFFF));
		printf("\tmtsp 1\n");
	} else
		repeated_op(size / 2, "pull 0");
	if (!(func_flags & F_VOIDRET))
		printf("\tsta 1,0,3\n");
	printf("\tret\n");
	unreachable = 1;
}

void gen_label(const char *tail, unsigned n)
{
	unreachable = 0;
	printf("L%d%s:\n", n, tail);
}

unsigned gen_exit(const char *tail, unsigned n)
{
	/* TODO: we need an ejmp that works out if it's in range as it often
	   will be doable relative */
	printf("\tjmp 1,1\n");
	printf("\t.word L%d%s\n", n, tail);
	unreachable = 1;
	return 0;
}

void gen_jump(const char *tail, unsigned n)
{
	printf("\tjmp L%d%s\n", n, tail);
}

void gen_jfalse(const char *tail, unsigned n)
{
	/* TODO we need a self expanding jump with value hiding */
	printf("\tjsr @__jf\n");
	printf("\t.word L%d%s\n", n, tail);
}

void gen_jtrue(const char *tail, unsigned n)
{
	printf("\tjsr @__jt\n");
	printf("\t.word L%d%s\n", n, tail);
}

void gen_switch(unsigned n, unsigned type)
{
	printf("\tjsr @switch,0\n");
	printf("\t.word Sw%d\n", n);
	unreachable = 1;
	/* Although we jsr that's just to pass the table ptr */
}

void gen_switchdata(unsigned n, unsigned size)
{
	printf("Sw%d:\n", n);
	printf("\t.word %d\n", size);
}

void gen_case_label(unsigned tag, unsigned entry)
{
	unreachable = 0;
	printf("Sw%d_%d:\n", tag, entry);
}

void gen_case_data(unsigned tag, unsigned entry)
{
	printf("\t.word Sw%d_%d\n", tag, entry);
}

void gen_helpcall(struct node *n)
{
	/* TODO need to figure out what is indirected this way and what
	   is done jsr 1,1 style */
	printf("\tjsr @__");
}

void gen_helpclean(struct node *n)
{
}

void gen_data_label(const char *name, unsigned align)
{
	printf("_%s:\n", name);
}

void gen_space(unsigned value)
{
	printf("\t.ds %d\n", value);
}

void gen_text_data(unsigned n)
{
	printf("\t.word T%d\n", n);
}

void gen_literal(unsigned n)
{
	if (n)
		printf("T%d:\n", n);
}

void gen_name(struct node *n)
{
	printf("\t.word _%s+%d\n", namestr(n->snum), WORD(n->value));
}

void gen_value(unsigned type, unsigned long value)
{
	if (PTR(type)) {
		printf("\t.word %u\n", (unsigned) value);
		return;
	}
	switch (type) {
	case CCHAR:
	case UCHAR:
	case CSHORT:
	case USHORT:
		printf("\t.word %d\n", (unsigned) value & 0xFFFF);
		break;
	case CLONG:
	case ULONG:
	case FLOAT:
		/* We are little endian - software choice */
		printf("\t.word %d\n", (unsigned) (value & 0xFFFF));
		printf("\t.word %d\n", (unsigned) ((value >> 16) & 0xFFFF));
		break;
	default:
		error("unsuported type");
	}
}

void gen_start(void)
{
	printf("\t.code\n");
}

void gen_end(void)
{
}

void gen_tree(struct node *n)
{
	codegen_lr(n);
	printf(";\n");
}

unsigned gen_push(struct node *n)
{
	/* Our push will put the object on the stack, so account for it */
	unsigned s = get_stack_size(n->type);
	sp += s / 2;
	if (s == 4) {
		printf("\tlda 0,__hireg, 0\n");
		printf("\tpush 0\n");
	} else
		printf("\tpush 1\n");
	printf(";\n");
	return 1;
}

static unsigned gen_constant(unsigned r, int16_t v)
{
	/* TODO: other values - to begin with byte swap forms of these */
	switch(v) {
	case 0:
		printf("\tsub %u,%u\n", r, r);
		return 1;
	case 1:
		printf("\tsubzl %u,%u\n", r, r);
		return 1;
	case 2:
		printf("\tsub %u,%u\n", r, r);
		printf("\tinczl %u,%u\n", r, r);
		return 1;
	case 3:
		printf("\tsub %u,%u\n", r, r);
		printf("\tincol %u,%u\n", r, r);
		return 1;
	case -1:
		printf("\tadc %u,%u\n", r, r);
		return 1;
	case -2:
		printf("\tadcz; %u,%u\n", r, r);
		return 1;
	}
	return 0;
}

/*
 *	True if we can load ac0 with the value we need without trashing
 *	AC1. This lets us avoid a lot of the pushing and popping we would
 *	otherwise do in the code generation. The more we can add to this
 *	the better. Right now we just do some ops but we could look at
 *	implementable subtrees even - eg deref of thing we can do or
 *	maths ops via AC2 on AC0
 */
static unsigned can_load_ac0(struct node *n)
{
	/* Start with simple stuff */
	unsigned s = get_size(n->type);
	if (s != 2)
		return 0;
	switch(n->type) {
	case T_LOCAL:
	case T_LREF:
	case T_NAME:
	case T_LABEL:
	case T_CONSTANT:
	case T_NREF:
	case T_LBREF:
		return 1;
	}
	return 0;
}
/*
 *	The actual helper can do more than can_load_ac0 but must either
 *	generate all the code or none. This allows some ops to try and
 *	see what is possible if they can recover from a "no" answer
 *
 *	TODO: teach it the constants we can magic up in 0
 */
static unsigned load_ac0(struct node *n)
{
	unsigned s = get_size(n->type);
	unsigned v = n->value;
	int16_t d = v;

	if (s != 2)
		return 0;
	switch(n->op) {
	case T_LOCAL:
		printf("\tmov 3,0\n");
		printf("\tlda 2,2,1\n");
		printf("\tadd 2,0,skp\n");
		printf("\t.word %d\n", d);
		return 1;
	case T_LREF:
		if (d >= -128 && d < 128) {
			printf("\tlda 0,%d,3\n", d);
			return 1;
		}
		printf("\tmov 3,2\n");
		printf("\tlda 0,2,1\n");
		printf("\tadd 0,2,skp\n");
		printf("\t.word %d\n", d);
		printf("\tlda 0,0,2\n");
		return 1;
	case T_CONSTANT:
		if (gen_constant(0, v) == 0) {
			printf("\tjsr @__const0\n");
			printf("\t.word %u\n", v);
		}
		return 1;
	case T_NAME:
		printf("\tjsr @__const0\n");
		printf("\t.word _%s+%u\n", namestr(n->snum), v);
		return 1;
	case T_LABEL:
		printf("\tjsr @__const0\n");
		printf("\t.word T%u+%u\n", n->val2, v);
		return 1;
	case T_NREF:
		printf("\tjsr @__iconst0\n");
		printf("\t.word _%s+%u\n", namestr(n->snum), v);
		return 1;
	case T_LBREF:
		printf("\tjsr @__iconst0\n");
		printf("\t.word T%u+%u\n", n->val2, v);
		return 1;
	}
	printf(";couldnt shortcut %x\n", n->op);
	return 0;
}

unsigned add_constant(uint16_t v)
{
	if (v == 0)
		return 1;
	if (v == 0xFFFF) {
		printf("\tneg 1,1\n");
		printf("\tcom 1,1\n");
		return 1;
	}
	printf("\tlda 0,2,1\n");
	printf("\tadd 0,1,skp\n");
	printf("\t.word %u\n", v & 0xFFFF);
	return 1;
}

/* Unless we can generate a value directly generate a helper call followed
   by the value for constants */
static unsigned const_condop(struct node *n, char *o, char *uo)
{
	if (get_size(n->type) == 4)
		return 0;
	
	if (n->op == T_CONSTANT && gen_constant(0, n->value))
		return 1;
	if (n->type & UNSIGNED)
		o = uo;
#if 0
	/* Not clear this is worth it */		
	if (gen_constant(0, n->value))
		return 2;
#endif		
	/* Constant didn't work, but we there are other wins */
	if (n->op != T_NAME && n->op != T_LABEL && n->op != T_CONSTANT)
		return 0;
	helper(n, o);
	gen_value(n->type, n->value);
	return 1;
}
/*
 *	If possible turn this node into a direct access. We've already checked
 *	that the right hand side is suitable. If this returns 0 it will instead
 *	fall back to doing it stack based.
 */
unsigned gen_direct(struct node *n)
{
	struct node *r = n->right;
	unsigned s = get_size(n->type);
	unsigned v;
	switch(n->op) {
	/* Clean up is special and must be handled directly. It also has the
	   type of the function return so don't use that for the cleanup value
	   in n->right */
	case T_CLEANUP:
		v = r->value / 2;
		if (v >= 5) {
			printf("\tmfsp 1\n");
			printf("\tlda 0,2,1\n");
			printf("\tadd 0,1,skip\n");
			printf("\t.word %u\n", (-v) & 0xFFFF);
			printf("\tmtsp 1\n");
		} else
			repeated_op(v, "pull 0");
		sp -= v;
		return 1;
	case T_PLUS:
		if (r->op == T_CONSTANT && s == 2) {
			v = r->value;
			if (add_constant(v))
				return 1;
		}
		if (load_ac0(r)) {
			printf("\tadd 0,1\n");
			return 1;
		}
		break;
	case T_MINUS:
		if (r->op == T_CONSTANT && s == 2) {
			v = r->value;
			if (add_constant(-v))
				return 1;
		}
		if (load_ac0(r)) {
			printf("\tsub 0,1\n");
			return 1;
		}
		break;
	/* These ops we can do at the least */ 
	case T_AND:
		if (r->op == T_CONSTANT && s == 2) {
			v = r->value;
			if (v == 0) {
				printf("\tsub 1,1\n");
				return 1;
			}
			if (v == 0xFFFF)
				return 1;
		}
		if (load_ac0(r)) {
			printf("\tand 0,1\n");
			return 1;
		}
		break;
	case T_OR:
		if (r->op == T_CONSTANT && s == 2) {
			v = r->value;
			if (v == 0)
				return 1;
			if (v == 0xFFFF) {
				printf("\tadc 1,1\n");	/* 0xFFFF */
				return 1;
			}
		}
		if (load_ac0(r)) {
			printf("\tcom 0,0\n");
			printf("\tand 0,1\n");
			printf("\tadc 0,1\n");
			return 1;
		}
		break;
	case T_HAT:
		if (r->op == T_CONSTANT && s == 2) {
			v = r->value;
			if (v == 0)
				return 1;
			if (v == 0xFFFF) {
				printf("\tcom 1,1\n");
				return 1;
			}
		}
		if (load_ac0(r)) {
			printf("\tmov 1,2\n");
			printf("\tandzl 0,2\n");
			printf("\tadd 0,1\n");
			printf("\tsub 2,1\n");
			return 1;
		}
		break;
	/* And some of the shift forms for constant */
	case T_LTLT:
	case T_GTGT:
		break;
	/* Plus some constant compares */
	case T_EQEQ:
		/* TODO: teach these all about the zero case shorter form */
		switch(const_condop(r, "condeq", "condeq")) {
		case 0:
			return 0;
		case 2:
			printf("\tsub 0,1,snr\n");
			printf("\tsubzl 1,1,skp\n");
			printf("\tsub 1,1\n");
		}
		n->flags |= ISBOOL;
		return 1;
	case T_BANGEQ:
		switch(const_condop(r, "condne", "condne")) {
		case 0:
			return 0;
		case 2:
			printf("\tsub 0,1,szr\n");
			printf("\tsubzl 1,1\n");
			/* if we skipped then AC1 is already zero */
		}
		n->flags |= ISBOOL;
		return 1;
	case T_LTEQ:
		switch(const_condop(r, "condlteq", "condltequ")) {
		case 0:
			return 0;
		case 2:
			printf("\tsubz# 0,1,snc\n");
			printf("\tsubzl 1,1,skp\n");
			printf("\tsub 1,1\n");
		}
		n->flags |= ISBOOL;
		return 1;
	case T_GT:
		switch(const_condop(r, "condgt", "condgt")) {
		case 0:
			return 0;
		case 2:
			printf("\tsubz# 1,0,snc\n");
			printf("\tsubzl 1,1,skp\n");
			printf("\tsub 1,1\n");
		}
		n->flags |= ISBOOL;
		return 1;
	case T_GTEQ:
		switch(const_condop(r, "condgteq", "condgteq")) {
		case 0:
			return 0;
		case 2:
			printf("\tadcz# 0,1,snc\n");
			printf("\tsubzl 1,1,skp\n");
			printf("\tsub 1,1\n");
		}
		n->flags |= ISBOOL;
		return 1;
	case T_LT:
		switch(const_condop(r, "condlt", "condltu")) {
		case 0:
			return 0;
		case 2:
			printf("\tadcz# 1,0,snc\n");
			printf("\tsubzl 1,1,skp\n");
			printf("\tsub 1,1\n");
		}
		n->flags |= ISBOOL;
		return 1;
	}
	return 0;
}

/*
 *	Allow the code generator to shortcut the generation of the argument
 *	of a single argument operator (for example to shortcut constant cases
 *	or simple name loads that can be done better directly)
 */
unsigned gen_uni_direct(struct node *n)
{
	return 0;
}

/*
 *	Allow the code generator to shortcut trees it knows
 */
unsigned gen_shortcut(struct node *n)
{
	unsigned s = get_size(n->type);
	struct node *l = n->left;
	struct node *r = n->right;
	unsigned nr = n->flags & NORETURN;

	/* Unreachable code we can shortcut into nothing ..bye.. */
	if (unreachable)
		return 1;
	/* The comma operator discards the result of the left side, then
	   evaluates the right. Avoid pushing/popping and generating stuff
	   that is surplus */
	if (n->op == T_COMMA) {
		l->flags |= NORETURN;
		codegen_lr(l);
		/* Parent determines child node requirements */
		r->flags |= nr;
		codegen_lr(r);
		return 1;
	}
	switch(n->op) {
	case T_PLUSPLUS:
		if (!nr)
			return 0;
	case T_PLUSEQ:	/* Very specific but common case */
		if ((l->op == T_LOCAL || l->op == T_ARGUMENT) && r->op == T_CONSTANT && r->value <= 2 && s == 2) {
			int d = l->value;
			if (r->value == 0 && nr)
				return 1;
			if (l->op == T_ARGUMENT)
				d -= frame_len + argbase;
			if (d < -128 || d >= 128)
				return 0;
			if (r->value > 0) {
				printf("\tisz %d,3\n", d);
				/* This might skip */
				printf("\tnop\n");
			}
			if (r->value == 2) {
				printf("\tisz %d,3\n", d);
				printf("\tnop\n");
			}
			if (!nr)
				printf("\tlda 1,%d,3\n", d);
			return 1;
		}
		break;
	}
	return 0;
}

unsigned gen_cast(struct node *n)
{
	unsigned lt = n->type;
	unsigned rt = n->right->type;
	unsigned ls;
	unsigned rs;

	if (PTR(rt))
		rt = USHORT;
	if (PTR(lt))
		lt = USHORT;

	/* Floats and stuff handled by helper */
	if (!IS_INTARITH(lt) || !IS_INTARITH(rt))
		return 0;

	ls = get_size(lt);
	rs = get_size(rt);

	printf(";cast to %x(%u) from %x(%u)\n", lt,ls, rt,rs);
	/* Size shrink is not always free as we work in words */
	if (ls <= rs) {
		if (ls == 1 && rs > 1) {	/* Need to mask */
			printf("\tlda 0,2,1\n");
			printf("\tand 0,N255,0\n");
		}
		return 1;
	}
	/* Don't do the harder ones */
	if (rs == 1 && !(rt & UNSIGNED))
		return 0;
	/* All byte ops are word ops internally and the save and load
	   mask so conversion from char should be free. To char unusually
	   on the other hand is not - see above */
	printf("\tsub 0,0\n");
	if (!(rt & UNSIGNED)) {
		/* If top bit set then set ac0 to -1 */
		printf("\tmovl# 1,1,snc\n");
		printf("\tadc 0,0\n");
	}
	printf("\tsta 0,__hireg,0\n");
	return 1;
}

unsigned gen_node(struct node *n)
{
	struct node *r = n->right;
	unsigned v = n->value;
	unsigned s = get_size(n->type);
	int16_t d = v;
	
	/* Function call arguments are special - they are removed by the
	   act of call/return and reported via T_CLEANUP */
	if (n->left && n->op != T_ARGCOMMA && n->op != T_FUNCCALL)
		sp -= get_stack_size(n->left->type) / 2;
	switch(n->op) {
	case T_CONSTANT:
		/* We should do longs using make constant twice */
		if (s < 4 && gen_constant(1, v))
			return 1;
	case T_NAME:
	case T_LABEL:
		v = n->value;
		if (s == 2)
			printf("\tjsr @__const\n");
		else
			printf("\tjsr @__constl\n");
		if (n->op == T_CONSTANT)
			gen_value(n->type, n->value);
		else if (n->op == T_NAME)
			printf("\t.word _%s+%u\n", namestr(n->snum), v);
		else
			printf("\t.word T%u+%u\n", n->val2, v);
		return 1;
	case T_NREF:
	case T_LBREF:
		/* Same logic but actual value */
		v = n->value;
		if (s == 2)
			printf("\tjsr @__iconst\n");
		else
			printf("\tjsr @__iconstl\n");
		if (n->op == T_NREF)
			printf("\t.word _%s_%u\n", namestr(n->snum), v);
		else
			printf("\t.word T%u+%u\n", n->val2, v);
		return 1;
	case T_NSTORE:
	case T_LBSTORE:
		/* Same logic but store  */
		v = n->value;
		if (s == 1 || s == 2)
			printf("\tjsr @__sconst\n");
		else
			printf("\tjsr @__sconstl\n");
		if (n->op == T_NREF)
			printf("\t.word _%s_%u\n", namestr(n->snum), v);
		else
			printf("\t.word T%u+%u\n", n->val2, v);
		return 1;
	case T_ARGUMENT:
		v -= argbase + frame_len;
	case T_LOCAL:
		if (s == 1)
			return 0;
		printf("\tmov 3,1\n");
		if (d) {
			printf("\tlda 0,2,1\n");
			printf("\tadd 1,0,skip\n");
			printf("\t.word %d\n", (int)d);
		}
		/* TODO maybe optimize generally "add const to ac" for
		   the size tricks that work */
		return 1;
	case T_LREF:
		/* An LREFPLUS rewrite might be useful to fold additions */
		/* TODO 4 is easy */
		if (d < 128 && d >= -128) {
			printf("\tlda 1,%d,3\n", (int)(int16_t)v);
			return 1;
		}
		/* Fix code dup with T_LOCAL */
		printf("\tmov 3,2\n");
		if (d) {
			printf("\tlda 0,2,1\n");
			printf("\tadd 0,2,skip\n");
			printf("\t.word %d\n", (int) d);
		}
		printf("\tlda 1,0,2\n");
		return 1;
	case T_LSTORE:
		if (d < 128 && d >= -128) {
			printf("\tsta 1,%d,3\n", (int)(int16_t)v);
			return 1;
		}
		/* Fix code dup with T_LOCAL */
		printf("\tmov 3,2\n");
		if (d) {
			printf("\tlda 0,2,1\n");
			printf("\tadd 0,2,skip\n");
			printf("\t.word %d\n", (int)d);
		}
		printf("\tsta 1,0,2\n");
		return 1;
	case T_DEREF:
		if (s == 1)
			return 0;
		printf("\tmov 1,2\n");
		if (s == 4) {
			printf("\tlda 1,2,2\n");
			printf("\tsta 1,__hireg,0\n");
		}
		printf("\tlda 1,0,2\n");
		return 1;
	case T_EQ:
		if (s == 1)	/* Byteops are hard */
			return 0;
		printf("\tpop 2\n");
		printf("\tsta 1,0,2\n");
		if (s == 4) {
			printf("\tlda 0,__hireg,0\n");
			printf("\tsta 0,0,2\n");
		}
		return 1;
	case T_BOOL:
		/* Bool and conditionals the size is the size on the right */
		/* Already bool ? */
		if (r->flags & ISBOOL)
			return 1;
		s = get_size(r->type);
		if (s == 4)
			return 0;
		if (s == 1) {
			printf("\tlda 0,__N255,0\n");
			printf("\tand 0,1\n");
		}
		printf("\tmov 1,1,szr\n");
		printf("\tsubzl 1,1\n");
		return 1;
	case T_BANG:
		/* TODO: could optimize bool not case to
		   com 1,1 inc 1,1 but we need to sort out the 
		   conditional jump story first */
		s = get_size(r->type);
		if (s == 4)
			return 0;
		if (s == 1) {
			printf("\tlda 0,__N255,0\n");
			printf("\tand 0,1\n");
		}
		printf("\tmov 1,1,snr\n");
		printf("\tsubzl 1,1,skp\n");
		printf("\tsub 1,1\n");
		return 1;
	case T_PLUS:
		printf("\tpop 0\n");
		printf("\tadd 0,1\n");
		if (s == 4) {
			printf("\tpop 2\n");
			printf("\tlda 0,__hireg,0\n");
			printf("\tadc 2,0\n");
			printf("\tsta 0,__hireg,0\n");
		}
		return 1;
	case T_MINUS:
		printf("\tpop 0\n");
		printf("\tsub 1,0\n");
		if (s == 4) {
			printf("\tpop 2\n");
			printf("\tlda 1,__hireg,0\n");
			printf("\tsub 1,2\n");
			printf("\tsta 2,__hireg,0\n");
		}
		printf("\tmov 0,1\n");
		return 1;
	case T_AND:
		printf("\tpop 0\n");
		printf("\tand 0,1\n");
		if (s == 4) {
			printf("\tpop 0\n");
			printf("\tlda 2,__hireg,0\n");
			printf("\tand 2,0\n");
			printf("\tsta 0,__hireg,0\n");
		}
		return 1;
	case T_OR:
		if (s == 4)
			return 0;
		printf("\tpop 0\n");
		printf("\tcom 0,0\n");
		printf("\tand 0,1\n");
		printf("\tadc 0,1\n");
		return 1;
	case T_HAT:
		printf("\tpop 0\n");
		printf("\tmov 1,2\n");
		printf("\tandzl 0,2\n");
		printf("\tadd 0,1\n");
		printf("\tsub 2,1\n");
		return 1;
	case T_EQEQ:
		if (s == 4)
			return 0;
		printf("\tpop 0\n");
		printf("\tsub 0,1,snr\n");
		printf("\tsubzl 1,1,skp\n");
		printf("\tsub 1,1\n");
		n->flags |= ISBOOL;
		return 1;
	case T_BANGEQ:
		if (s == 4)
			return 0;
		printf("\tpop 0\n");
		printf("\tsub 0,1,szr\n");
		printf("\tsubzl 1,1\n");
		/* if we skipped then AC1 is already zero */
		n->flags |= ISBOOL;
		return 1;
	case T_CAST:
		return gen_cast(n);
	/* These ops we can do at the least for some forms */
	/* Apart from the pop keep them the same as the direct ones
	   as we will need to use peephole rules to fuse them with conditional
	   jumps, and also need CCONLY support in some cases to rewrite them
	   nicely */
	case T_LTEQ:
		s = get_size(r->type);
		if (s != 4 && (n->type & UNSIGNED)) {
			printf("\tpop 0\n");
			printf("\tsubz# 0,1,snc\n");
			printf("\tsubzl 1,1,skp\n");
			printf("\tsub 1,1\n");
			n->flags |= ISBOOL;
			return 1;
		}
		break;
	case T_GT:
		s = get_size(r->type);
		if (s != 4 && (n->type & UNSIGNED)) {
			printf("\tpop 0\n");
			printf("\tsubz# 1,0,snc\n");
			printf("\tsubzl 1,1,skp\n");
			printf("\tsub 1,1\n");
			n->flags |= ISBOOL;
			return 1;
		}
		break;
	case T_GTEQ:
		s = get_size(r->type);
		if (s != 4 && (n->type & UNSIGNED)) {
			printf("\tpop 0\n");
			printf("\tadcz# 0,1,snc\n");
			printf("\tsubzl 1,1,skp\n");
			printf("\tsub 1,1\n");
			n->flags |= ISBOOL;
			return 1;
		}
		break;
	case T_LT:
		s = get_size(r->type);
		if (s != 4 && (n->type & UNSIGNED)) {
			printf("\tpop 0\n");
			printf("\tadcz# 1,0,snc\n");
			printf("\tsubzl 1,1,skp\n");
			printf("\tsub 1,1\n");
			n->flags |= ISBOOL;
			return 1;
		}
		break;
	}
	return 0;
}
