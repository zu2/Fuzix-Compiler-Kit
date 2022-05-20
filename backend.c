/*
 *	This is the main block for the code generator. It provides the
 *	basic parsing functions to make life easy for the target code
 *	generator. A target is not required to use this, it can work the
 *	tree/header mix any way it wants.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include "symtab.h"
#include "compiler.h"
#include "backend.h"

int sym_fd = -1;

unsigned cpu;
char opt;

static unsigned process_one_block(uint8_t *h);

static const char *argv0;

void error(const char *p)
{
	fprintf(stderr, "%s: error: %s\n", argv0, p);
	exit(1);
}

static void xread(int fd, void *buf, int len)
{
	if (read(fd, buf, len) != len)
		error("short read");
}

/*
 *	Name symbol table.
 *
 *	We can just cache bits of this in cc2 if we actually get tight on
 *	space. It's not a big deal as we only use names for global and static
 *	objects.
 */

#define NCACHE_SIZE	32
static struct name names[NCACHE_SIZE];
static struct name *nhead;
static unsigned max_name;

char *namestr(unsigned n)
{
	struct name *np = nhead;
	struct name *prev = NULL;
	while(np) {
		if (np->id == n) {
			if (prev) {
				prev->next = np->next;
				np->next = nhead;
				nhead = np;
			}
			return np->name;
		}
		prev = np;
		np = np->next;
	}
	/* Hack for now we need to pick a better node */
	if (lseek(sym_fd, 2 + sizeof(struct name) * (n & 0x7FFF), 0) < 0)
		error("seeksym");
	xread(sym_fd, prev, sizeof(struct name));
	prev->next = NULL;
	return prev->name;
}

static void init_name_cache(void)
{
	unsigned i;
	struct name *np = names;
	for (i = 0; i < NCACHE_SIZE - 1; i++) {
		np->next = np + 1;
		np++;
	}
	np->next = NULL;
	nhead = names;
}

/*
 *	Expression tree nodes
 */
#define NUM_NODES 100

static struct node node_table[NUM_NODES];
static struct node *nodes;

struct node *new_node(void)
{
	struct node *n;
	if (nodes == NULL)
		error("Too many nodes");
	n = nodes;
	nodes = n->right;
	n->left = n->right = NULL;
	n->value = 0;
	n->flags = 0;
	return n;
}

void free_node(struct node *n)
{
	n->right = nodes;
	nodes = n;
}

void init_nodes(void)
{
	int i;
	struct node *n = node_table;
	for (i = 0; i < NUM_NODES; i++)
		free_node(n++);
}

void free_tree(struct node *n)
{
	if (n->left)
		free_tree(n->left);
	if (n->right)
		free_tree(n->right);
	free_node(n);
}

/* Small stack of segments so we can untangle literals etc */

static unsigned segs[MAX_SEG];
static unsigned *segp = segs;
static unsigned last_seg;

static void push_area(unsigned s)
{
	if (segp == &segs[MAX_SEG])
		error("pua");
	*segp++ = last_seg;
	if (last_seg != s) {
		gen_segment(s);
		last_seg = s;
	}
}

static void pop_area(void)
{
	if (segp == segs)
		error("poa");
	segp--;
	/* Last pop is to "nothing" so we don't need to act */
	if (segp > segs && last_seg != *segp) {
		gen_segment(*segp);
		last_seg = *segp;
	}
}

/* I/O buffering stuff can wait - as can switching to a block write method */
static struct node *load_tree(void)
{
	struct node *n = new_node();
	xread(0, n, sizeof(struct node));

	/* The values off disk are old pointers or NULL, that's good enough
	   to use as a load flag */
	if (n->left)
		n->left = load_tree();
	if (n->right)
		n->right = load_tree();
	return n;
}

static struct node *rewrite_tree(struct node *n)
{
	if (n->left)
		n->left = rewrite_tree(n->left);
	if (n->right)
		n->right = rewrite_tree(n->right);
	/* Convert LVAL flag into pointer type */
	if (n->flags & LVAL)
		n->type++;
	if (!PTR(n->type) && n->type >= 0x4000)
		fprintf(stderr, "bad node type %x for node of %x\n",
			n->type, n->op);
	/* Turn any remaining object references (functions) into pointer type */
	/* Need to review how we do this with name of function versus function vars etc */
	/* FIXME */
	if (IS_FUNCTION(n->type))
		n->type = PTRTO;
	return gen_rewrite_node(n);
}

static unsigned process_expression(void)
{
	struct node *n = rewrite_tree(load_tree());
	unsigned t;
	gen_tree(n);
	t = n->type;
	free_tree(n);
	return t;
}

static unsigned compile_expression(void)
{
	uint8_t h[2];
	unsigned t;
	/* We can end up with literal headers before the expression if the
	   expression is something like if (x = "eep"). Process up to and
	   including our expression */
	do {
		xread(0, h, 2);
		t = process_one_block(h);
	} while(h[1] != '^');
	return t;
}

/*
 *	Process the header blocks. We call out to the target to let it
 *	handle the needs of the platform.
 */

static unsigned func_ret;
static unsigned frame_len;
static unsigned func_ret_used;

static void process_literal(unsigned id)
{
	unsigned char c;
	unsigned char shifted = 0;

	gen_literal(id);

	/* A series of bytes terminated by a 0 marker. Internal
	   zero is quoted, undo the quoting and turn it into data */
	while(1) {
		if (read(0, &c, 1) != 1)
			error("unexpected EOF");
		if (c == 0) {
			break;
		}
		if (c == 255 && !shifted) {
			shifted = 1;
			continue;
		}
		if (shifted && c == 254)
			c = 0;
		shifted = 0;
		gen_value(UCHAR, c);
	}
}

static void process_header(void)
{
	struct header h;
	xread(0, &h, sizeof(struct header));

	switch (h.h_type) {
	case H_EXPORT:
		gen_export(namestr(h.h_name));
		break;
	case H_FUNCTION:
		push_area(A_CODE);
		gen_prologue(namestr(h.h_data));
		func_ret = h.h_name;
		func_ret_used = 0;
		break;
	case H_FRAME:
		frame_len = h.h_name;
		gen_frame(h.h_name);
		break;
	case H_FUNCTION | H_FOOTER:
		if (func_ret_used)
			gen_label("_r", h.h_name);
		gen_epilogue(frame_len);
		pop_area();
		break;
	case H_FOR:
		compile_expression();
		/* We will loop back to the conditional */
		gen_label("_l", h.h_data);
		compile_expression();
		/* Exit the loop if false */
		gen_jfalse("_b", h.h_data);
		/* Jump top the main body if not */
		gen_jump("_n", h.h_data);
		/* We continue with the final clause of the for */
		gen_label("_c", h.h_data);
		compile_expression();
		/* Then jump to the condition */
		gen_jump("_l", h.h_data);
		/* Body starts here */
		gen_label("_n", h.h_data);
		break;
	case H_FOR | H_FOOTER:
		gen_jump("_c", h.h_data);
		gen_label("_b", h.h_data);
		break;
	case H_WHILE:
		gen_label("_c", h.h_data);
		compile_expression();
		gen_jfalse("_b", h.h_data);
		break;
	case H_WHILE | H_FOOTER:
		gen_jump("_c", h.h_data);
		gen_label("_b", h.h_data);
		break;
	case H_DO:
		gen_label("_c", h.h_data);
		break;
	case H_DOWHILE:
		compile_expression();
		gen_jtrue("_c", h.h_data);
		break;
	case H_DO | H_FOOTER:
		gen_jump("_c", h.h_data);
		gen_label("_b", h.h_data);
		break;
	case H_BREAK:
		gen_jump("_b", h.h_name);
		break;
	case H_CONTINUE:
		gen_jump("_c", h.h_name);
		break;
	case H_IF:
		compile_expression();
		gen_jfalse("_e", h.h_name);
		break;
	case H_ELSE:
		gen_jump("_f", h.h_name);
		gen_label("_e", h.h_name);
		break;
	case H_IF | H_FOOTER:
		/* If we have an else then _f is needed, if not _e is */
		if (h.h_data)
			gen_label("_f", h.h_name);
		else
			gen_label("_e", h.h_name);
		break;
	case H_RETURN:
		func_ret_used = 1;
		break;
	case H_RETURN | H_FOOTER:
		gen_jump("_r", func_ret);
		break;
	case H_LABEL:
		gen_label("", h.h_name);
		break;
	case H_GOTO:
		/* TODO - goto might be special ? */
		gen_jump("", h.h_name);
		break;
	case H_SWITCH:
		/* Generate the switch header, expression and table run */
		gen_switch(h.h_name, compile_expression());	/* need the type of it back */
		break;
	case H_CASE:
		gen_case_label(h.h_name, h.h_data);
		break;
	case H_DEFAULT:
		gen_case_label(h.h_name, 0);
		break;
	case H_SWITCH | H_FOOTER:
		gen_label("_b", h.h_data);
		break;
	case H_SWITCHTAB:
		push_area(A_LITERAL);
		gen_switchdata(h.h_name, h.h_data);
		break;
	case H_SWITCHTAB | H_FOOTER:
		pop_area();
		break;
	case H_DATA:
		push_area(A_DATA);
		gen_data_label(namestr(h.h_name), h.h_data);
		break;
	case H_DATA | H_FOOTER:
		pop_area();
		break;
	case H_BSS:
		push_area(A_BSS);
		gen_data_label(namestr(h.h_name), h.h_data);
		break;
	case H_BSS | H_FOOTER:
		pop_area();
		break;
	case H_STRING:
		push_area(A_LITERAL);
		process_literal(h.h_name);
		break;
	case H_STRING| H_FOOTER:
		pop_area();
		break;
	default:
		error("bad hdr");
		break;
	}
}

/* Each data node is a one node tree right now. We ought to trim this down
   to avoid bloating the intermediate file */

void process_data(void)
{
	struct node *n = load_tree();
	switch (n->op) {
	case T_PAD:
		gen_space(n->value);
		break;
	case T_LABEL:
		gen_text_data(n->value);
		break;
	case T_NAME:
		gen_name(n);
		break;
	case T_CASELABEL:
		gen_case_data(n->value, n->val2);
		break;
	default:
		gen_value(n->type, n->value);
		break;
	}
	free_node(n);
}

/*
 *	Helpers for the code generation whenever the target has no
 *	direct method
 */

void helper_type(unsigned t)
{
	if (PTR(t))
		t = CSHORT;
	switch (t) {
	case UCHAR:
		putchar('u');
	case CCHAR:
		putchar('c');
		break;
	case UINT:
		putchar('u');
	case CSHORT:
		break;
	case ULONG:
		putchar('u');
	case CLONG:
		putchar('l');
		break;
	case FLOAT:
		putchar('f');
		break;
	case DOUBLE:
		putchar('d');
		break;
	default:
		fflush(stdout);
		fprintf(stderr, "*** bad type %x\n", t);
	}
}

/*
 *	Generate a helper call according to the types
 *
 *	Would be nice to have an option to build C like helper calls
 */
void do_helper(struct node *n, const char *h, unsigned t)
{
	/* A function call has a type that depends upon the call, but the
	   type we want is a pointer */
	if (n->op == T_FUNCCALL)
		n->type = PTRTO;
	gen_helpcall(n);
	fputs(h, stdout);
	if (n->op == T_CAST) {
		helper_type(n->right->type);
		putchar('_');
	}
	helper_type(t);
	putchar('\n');
	gen_helpclean(n);
}

void helper(struct node *n, const char *h)
{
	do_helper(n, h, n->type & ~UNSIGNED);
}

/* Sign of types matters */
void helper_s(struct node *n, const char *h)
{
	do_helper(n, h, n->type);
}

void make_node(struct node *n)
{
	/* Try the target code generator first, if not use helpers */
	if (gen_node(n))
		return;

	switch (n->op) {
	case T_NULL:
		/* Dummy 'no expression' node */
		break;
	case T_SHLEQ:
		helper(n, "shleq");
		break;
	case T_SHREQ:
		helper_s(n, "shreq");
		break;
	case T_PLUSPLUS:
		/* Avoid the post op cost if the result isn't used, as is
		   commonly the case */
		if (n->flags & NORETURN)
			helper(n, "pluseq");
		else
			helper(n, "postinc");
		break;
	case T_MINUSMINUS:
		if (n->flags & NORETURN)
			helper(n, "minuseq");
		else
			helper(n, "postdec");
		break;
	case T_EQEQ:
		helper(n, "cceq");
		n->flags |= ISBOOL;
		break;
	case T_LTLT:
		helper(n, "shl");
		break;
	case T_GTGT:
		helper_s(n, "shr");
		break;
	case T_OROR:
		/* Handled with branches in the tree walk */
		break;
	case T_ANDAND:
		/* Handled with branches in the tree walk */
		break;
	case T_PLUSEQ:
		helper(n, "pluseq");
		break;
	case T_MINUSEQ:
		helper(n, "minuseq");
		break;
	case T_SLASHEQ:
		helper_s(n, "diveq");
		break;
	case T_STAREQ:
		helper(n, "muleq");
		break;
	case T_HATEQ:
		helper(n, "xoreq");
		break;
	case T_BANGEQ:
		helper(n, "ccne");
		break;
	case T_OREQ:
		helper(n, "oreq");
		break;
	case T_ANDEQ:
		helper(n, "andeq");
		break;
	case T_PERCENTEQ:
		helper_s(n, "modeq");
		break;
	case T_AND:
		helper(n, "band");
		break;
	case T_STAR:
		helper(n, "mul");
		break;
	case T_SLASH:
		helper_s(n, "div");
		break;
	case T_PERCENT:
		helper_s(n, "mod");
		break;
	case T_PLUS:
		helper(n, "plus");
		break;
	case T_MINUS:
		helper(n, "minus");
		break;
	case T_COLON:
	case T_QUESTION:
		/* We did the work in the code generator as it's not a simple
		   operator behaviour */
		break;
	case T_HAT:
		helper(n, "xor");
		break;
	case T_LT:
		helper_s(n, "cclt");
		n->flags |= ISBOOL;
		break;
	case T_GT:
		helper_s(n, "ccgt");
		n->flags |= ISBOOL;
		break;
	case T_LTEQ:
		helper_s(n, "cclteq");
		n->flags |= ISBOOL;
		break;
	case T_GTEQ:
		helper_s(n, "ccgteq");
		n->flags |= ISBOOL;
		break;
	case T_OR:
		helper(n, "or");
		break;
	case T_TILDE:
		helper(n, "cpl");
		break;
	case T_BANG:
		helper(n, "not");
		n->flags |= ISBOOL;
		break;
	case T_EQ:
		helper(n, "assign");
		break;
	case T_DEREF:
		helper(n, "deref");
		break;
	case T_NEGATE:
		helper(n, "negate");
		break;
	case T_FUNCCALL:
		helper(n, "callfunc");
		break;
	case T_CLEANUP:
		/* Should never occur except direct */
		error("tclu");
		break;
	case T_LABEL:
		helper(n, "const");
		/* Used for const strings */
		gen_text_data(n->value);
		break;
	case T_CAST:
		helper_s(n, "cast");
		break;
	case T_CONSTANT:
		helper(n, "const");
		gen_value(n->type, n->value);
		break;
	case T_COMMA:
		/* foo, bar - we evaulated foo and stacked it, now throw it away */
		/* Targets will normally shortcut this push/pop or peephole it */
		helper(n, "pop");
		break;
	case T_ARGCOMMA:
		/* Used for function arg chaining - just ignore */
		return;
	case T_BOOL:
		/* Check if we know it's already bool */
		if (n->right && (n->right->flags & ISBOOL))
			break;
		helper(n, "bool");
		break;
	case T_NAME:
		helper(n, "loadn");
		gen_name(n);
		break;
	case T_LOCAL:
		helper(n, "loadl");
		gen_value(PTRTO, n->value);
		break;
	case T_ARGUMENT:
		helper(n, "loada");
		gen_value(PTRTO, n->value);
		break;
	default:
		fprintf(stderr, "Invalid %04x\n", n->op);
		exit(1);
	}
}
/*
 *	Load the symbol table from the front end
 */

static void load_symbols(const char *path)
{
	sym_fd = open(path, O_RDONLY);
	uint8_t n[2];
	if (sym_fd == -1) {
		perror(path);
		exit(1);
	}
	xread(sym_fd, n, 2);
	max_name = n[0] | (n[1] << 8);
}

static unsigned process_one_block(uint8_t *h)
{
	if (h[0] != '%')
		error("sync");
	/* We write a sequence of records starting %^ for an expression
	   %[ for data blocks and %H for a header. This helps us track any
	   errors and sync screwups when parsing */
	if (h[1] == '^')
		return process_expression();
	else if (h[1] == 'H')
		process_header();
	else if (h[1] == '[')
		process_data();
	else
		error("unknown block");
	return 0;
}

/*
 *	Helpers for the targets
 */

static unsigned codegen_label;

/*
 *	Some 'expressions' are actually flow changing things disguised
 *	as expressions. Deal with them above the processor specific level.
 */
static unsigned branching_operator(struct node *n)
{
	if (n->op == T_OROR)
		return 1;
	if (n->op == T_ANDAND)
		return 2;
	if (n->op == T_COLON)
		return 3;
	if (n->op == T_QUESTION)
		return 4;
	return 0;
}

/*
 *	Perform a simple left right walk of the tree and feed the code
 *	to the node generator.
 */
void codegen_lr(struct node *n)
{
	unsigned o = branching_operator(n);

	/* Certain operations require special handling because the rule is
	   for partial evaluation only. Notably && || and ?: */
	if (o) {
		unsigned lab = codegen_label++;
		/*  foo ? a : b is a strange beast. At this point we have
		    foo in the work register so need do nothing, and let the
		    ? subtree resolve it */
		if (o == 4)
			return;
		if (o == 3) {
			gen_jfalse("L", lab);
			codegen_lr(n->left);
			gen_jump("LC", lab);
			gen_label("L", lab);
			codegen_lr(n->right);
			gen_label("LC", lab);
			make_node(n);
			return;
		}
		/* TODO ? shortcut && and || if one side is constant */
		codegen_lr(n->left);
		if (o == 1)
			gen_jtrue("L", lab);
		else
			gen_jfalse("L", lab);
		codegen_lr(n->right);
		gen_label("L", lab);
		/* We don't build the node itself - it's not relevant */
		return;
	}

	/* Allow the code generator to short cut the tree walk for things it
	   knows how to directly complete */
	if (gen_shortcut(n))
		return;

	if (n->left) {
		codegen_lr(n->left);
		/* See if we can direct generate this block. May recurse */
		if (gen_direct(n))
			return;
		if (!gen_push(n->left))
			helper(n->left, "push");
	} else {
		/* Single argument hook to generate stuff without pre-loading
		   right into working register */
		if (gen_uni_direct(n))
			return;
	}
	if (n->right)
		codegen_lr(n->right);
	make_node(n);
}

/*
 *	Entry point
 */

int main(int argc, char *argv[])
{
	uint8_t h[2];

	argv0 = argv[0];

	/* We can make this better later */
	if (argc != 4)
		error("arguments");
	cpu = atoi(argv[2]);
	opt = *argv[3];
	init_name_cache();
	load_symbols(argv[1]);
	init_nodes();

	gen_start();
	while (read(0, &h, 2) > 0) {
		process_one_block(h);
	}
	gen_end();
}
