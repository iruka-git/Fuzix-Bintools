/*
 * 8086 assembler.
 * Assemble one line of input.
 * Knows all the dirt.
 */
#include	"as.h"

/*
 * CPU specific pass setup
 */

static int cputype;
/* FIXME: we should malloc/realloc this on non 8bit machines */
static uint8_t reltab[1024];
static unsigned int nextrel;

int passbegin(int pass)
{
	cputype = 8086;
	segment = 1;		/* Default to code */
	if (pass == 3)
		nextrel = 0;
	return 1;		/* All passes required */
}

static void setnextrel(int flag)
{
	if (nextrel == 8 * sizeof(reltab))
		aerr(TOOMANYJCC);
	if (flag)
		reltab[nextrel >> 3] |= (1 << (nextrel & 7));
	nextrel++;
}

static unsigned int getnextrel(void)
{
	unsigned int n = reltab[nextrel >> 3] & (1 << (nextrel & 7));
	nextrel++;
	return n;
}

/* Handle the corner case of labels in direct page being used as relative
   branches from the overlapping 'absolute' space */
static int segment_incompatible(ADDR *ap)
{
	if (ap->a_segment == segment)
		return 0;
	return 1;
}

static void constify(ADDR *ap)
{
	if ((ap->a_type & TMMODE) == (TUSER|TMINDIR))
		ap->a_type = TUSER;
}

void write_rel16(ADDR *ap)
{
	/* Absolute value - outraw will write it as is */
	if (ap->a_segment == ABSOLUTE) {
		outraw(ap);
		return;
	}
	ap->a_value -= dot[segment];
	ap->a_value -= 2;
	outrawrel(ap);
}

/* Simple expressions use this helper */
void getaddr(ADDR *ap)
{
	int c;

	ap->a_type = 0;
	ap->a_flags = 0;
	ap->a_sym = NULL;

	c = getnb();

	if (c == '<')
		ap->a_flags |= A_LOW;
	else if (c == '>')
		ap->a_flags |= A_HIGH;
	else
		unget(c);
	expr1(ap, LOPRI, 0);
}

/*
 *	Complex encodings.
 *
 *	reg8
 *	reg16
 *	[BX]
 *	[DI]
 *	[SI]
 *	[addr.16]
 *	[BP+SI]
 *	[BP+DI]
 *	[BX+SI]
 *	[BX+DI]
 *	[BP+SI+i8/16]
 *	[BP+DI+i8/16]
 *	[BX+SI+i8/16]
 *	[BX+DI+i8/16]
 *	[BP+i8/16]
 *	[BX+i8/16]
 *	[DI+i8/16]
 *	[SI+i8/16]
 *
 */

/* Registers within address specifications. Only BX BP SI DI */
static unsigned get_reg(void)
{
	/* Not quite right as it accepts  b x */
	int c = getnb();
	int c2 =  getnb();
	if (c == 'b') {
		if (c2 == 'x')
			return BX;
		if (c2 == 'p')
			return BP;
	} else if (c2 == 'i') {
		if (c == 's')
			return SI;
		if (c == 'd')
			return DI;
	}
	unget(c2);
	unget(c);
	return 0;
}

/* Turn a pair of reg names and a value into something */
static unsigned make_modrm(ADDR *ap, unsigned r1, unsigned r2)
{
	unsigned modrm;
	int off;

	if (r1 == BX) {
		if (r2 == SI)
			modrm = 0;
		else if (r2 == DI)
			modrm = 1;
		else if (r2 == 0)
			modrm = 7;
		else
			aerr(BADMODE);
	} else if (r1 == BP) {
		if (r2 == SI)
			modrm = 2;
		if (r2 == DI)
			modrm = 3;
		else if (r2 == 0)
			modrm = 6;
		else
			aerr(BADMODE);
	} else if (r1 == SI)
		modrm = 4;
	else if (r1 == SI)
		modrm = 5;
	else
		aerr(BADMODE);
	/* Now figure the mod bits */
	/* TODO: deal with unresolved symbols as 16bit with a relocation */
	off = ap->a_value;
	if (off < -128 || off > 127)
		modrm |= 0x80;
	else if (off != 0 || modrm == 6)
		modrm |= 0x40;
	return modrm;
}

/* TODO: segment prefixing, byte/word hints */
void getaddr_mem(ADDR *ap, unsigned *modrm)
{
	int c;
	unsigned mem = 0;
	unsigned mod = 0;
	unsigned r1 = 0, r2 = 0;

	ap->a_type = 0;
	ap->a_flags = 0;
	ap->a_sym = NULL;

	c = getnb();
	if (c == '[') {
		mem = 1;
		c = getnb();
	}
	if (c == '<')
		ap->a_flags |= A_LOW;
	else if (c == '>')
		ap->a_flags |= A_HIGH;
	else
		unget(c);

	if (mem == 0)  {
		/* Can be a register - how to handle nicely ? */
		expr1(ap, LOPRI, 0);
		if ((ap->a_type & TMMODE) == TBR) {
			*modrm = 0xC0 | (ap->a_type & TMREG);
			/* 8bit implied */
		} else if ((ap->a_type & TMMODE) == TWR) {
			*modrm = 0xC0 | (ap->a_type & TMREG);
			/* 16bit implied */
		}
		else if ((ap->a_type & TMMODE) == TSR) {
			/* Special */
		} else
			ap->a_type |= TIMMED;
		return;
	}

	/* Is it a register  ? */
	r1 = get_reg();
	if (r1 == 0) {
		/* Simple expression for the address */
		expr1(ap, LOPRI, 0);
		mod = 0x06;
	} else {
		c = getnb();
		if (c == '+') {
			r2 = get_reg();
			/* [ Reg + expr ] */
			if (r2 == -1) {
				/* Expression then ? */
				expr1(ap, LOPRI, 0);
				mod = make_modrm(ap, r1, 0);
			} else {
				/* [ Reg + Reg .. */
				c = getnb();
				/* [ Reg + Reg + num] */
				if (c == '+') {
					expr1(ap, LOPRI, 0);
					mod = make_modrm(ap, r1, r2);
				} else  {
					unget(c);
					ap->a_value = 0;
					mod = make_modrm(ap, r1, r2);
				}
			}
		} else {
			/* Single register */
			unget(c);
			ap->a_value = 0;
			mod = make_modrm(ap, r1, 0);
		}
	}
	c = getnb();
	if (c != ']')
		aerr(SQUARE_EXPECTED);
	ap->a_type |= TMODRM;
	*modrm = mod;
}

static void outmod(unsigned modrm, ADDR *ap)
{
	outab(modrm);
	switch(modrm & 0xC0) {
	case 0x00:	/* no offset, but also includes direct address */
		if ((modrm & 7) == 6)
			outraw(ap);
		break;
	case 0x40:	/* 8bit offset */
		/* TODO use low of 16bit */
		outab(ap->a_value);
		break;
	case 0x80:	/* 16bit offset */
		outraw(ap);
		break;
	}
}

static unsigned opsize(unsigned modrm, ADDR *ap)
{
	if ((ap->a_type & TMMODE) == TWR)
		return 1;	/* word */
	if ((ap->a_type & TMMODE) == TBR)
		return 0;	/* byte */
	if ((ap->a_type & TMMODE) == TSR)
		return 1;	/* segment reg */
	/* TODO byte/word overrides */
	return 0;
}

static void need_186(void)
{
	if (cputype < 186)
		qerr(NEED_186);
}

/*
 * Assemble one line.
 * The line in in "ib", the "ip"
 * scans along it. The code is written
 * right out.
 */
void asmline(void)
{
	SYM *sp;
	int c;
	int opcode;
	int disp;
	int reg;
	int srcreg;
	int cc;
	VALUE value;
	int delim;
	SYM *sp1;
	char id[NCPS];
	char id1[NCPS];
	ADDR a1;
	ADDR a2;
	unsigned ta1, ta2;
	unsigned mod1;
	unsigned mod2;

loop:
	if ((c=getnb())=='\n' || c==';')
		return;
	if (isalpha(c) == 0 && c != '_' && c != '.')
		qerr(UNEXPECTED_CHR);
	getid(id, c);
	if ((c=getnb()) == ':') {
		sp = lookup(id, uhash, 1);
		if (pass == 0) {
			if ((sp->s_type&TMMODE) != TNEW
			&&  (sp->s_type&TMASG) == 0)
				sp->s_type |= TMMDF;
			sp->s_type &= ~TMMODE;
			sp->s_type |= TUSER;
			sp->s_value = dot[segment];
			sp->s_segment = segment;
		} else if (pass != 3) {
			/* Don't check for duplicates, we did it already
			   and we will confuse ourselves with the pass
			   before. Instead blindly update the values */
			sp->s_type &= ~TMMODE;
			sp->s_type |= TUSER;
			sp->s_value = dot[segment];
			sp->s_segment = segment;
		} else {
			/* Phase 2 defined the values so a misalignment here
			   is fatal */
			if ((sp->s_type&TMMDF) != 0)
				err('m', MULTIPLE_DEFS);
			if (sp->s_value != dot[segment])
				err('p', PHASE_ERROR);
		}
		goto loop;
	}
	/*
	 * If the first token is an
	 * id and not an operation code,
	 * assume that it is the name in front
	 * of an "equ" assembler directive.
	 */
	if ((sp=lookup(id, phash, 0)) == NULL) {
		getid(id1, c);
		if ((sp1=lookup(id1, phash, 0)) == NULL
		||  (sp1->s_type&TMMODE) != TEQU) {
			err('o', SYNTAX_ERROR);
			return;
		}
		getaddr(&a1);
		constify(&a1);
		istuser(&a1);
		sp = lookup(id, uhash, 1);
		if ((sp->s_type&TMMODE) != TNEW
		&&  (sp->s_type&TMASG) == 0)
			err('m', MULTIPLE_DEFS);
		sp->s_type &= ~(TMMODE|TPUBLIC);
		sp->s_type |= TUSER|TMASG;
		sp->s_value = a1.a_value;
		sp->s_segment = a1.a_segment;
		/* FIXME: review .equ to an external symbol/offset and
		   what should happen */
		goto loop;
	}
	unget(c);
	opcode = sp->s_value;
	switch (sp->s_type&TMMODE) {
	case TORG:
		getaddr(&a1);
		constify(&a1);
		istuser(&a1);
		if (a1.a_segment != ABSOLUTE)
			qerr(MUST_BE_ABSOLUTE);
		outsegment(ABSOLUTE);
		dot[segment] = a1.a_value;
		/* Tell the binary generator we've got a new absolute
		   segment. */
		outabsolute(a1.a_value);
		break;

	case TEXPORT:
		getid(id, getnb());
		sp = lookup(id, uhash, 1);
		sp->s_type |= TPUBLIC;
		break;
		/* .code etc */

	case TSEGMENT:
		segment = sp->s_value;
		/* Tell the binary generator about a segment switch to a non
		   absolute segnent */
		outsegment(segment);
		break;

	case TDEFB:
		do {
			getaddr(&a1);
			constify(&a1);
			istuser(&a1);
			outrab(&a1);
		} while ((c=getnb()) == ',');
		unget(c);
		break;

	case TDEFW:
		do {
			getaddr(&a1);
			constify(&a1);
			istuser(&a1);
			outraw(&a1);
		} while ((c=getnb()) == ',');
		unget(c);
		break;

	case TDEFM:
		if ((delim=getnb()) == '\n')
			qerr(MISSING_DELIMITER);
		while ((c=get()) != delim) {
			if (c == '\n')
				qerr(MISSING_DELIMITER);
			outab(c);
		}
		break;

	case TDEFS:
		getaddr(&a1);
		constify(&a1);
		istuser(&a1);
		/* Write out the bytes. The BSS will deal with the rest */
		for (value = 0 ; value < a1.a_value; value++)
			outab(0);
		break;

	case TPREFIX:
		outab(opcode);
		goto loop;

	case T186IMPL:
		need_186();
		/* Fall through */
	case TIMPL:
		if (opcode >> 8)
			outab(opcode >> 8);
		outab(opcode);
		break;

	case TJCX:
		getaddr(&a1);
		disp = a1.a_value - dot[segment] - 2;
		if (disp <-128 || disp > 127)
			aerr(JMP_RANGE);
		outab(opcode);
		outab(disp);
		break;

	case TJCC:
		getaddr(&a1);
		disp = a1.a_value - dot[segment] - 2;
		if (pass == 3)
			c = getnextrel();
		else {
			c = 0;
			/* Will it fit, do we know ? */
			if (pass == 0 || segment_incompatible(&a1) || disp<-128 || disp>127)
				c = 1;
			/* On pass 2 we lock down our choices in the table */
			if (pass == 2)
				setnextrel(c);
		}
		if (c) {	/* LBxx is 0x10, Bxx, ... */
			if (opcode == 0x8D)	/* BSR -> LBSR */
				outab(0x17);
			else if (opcode == 0x20) /* BRA -> LBRA */
				outab(0x16);
			else {	/* Conditional ones expand with 0x10 */
				outab(0x10);
				outab(opcode);
			}
			write_rel16(&a1);
		} else {
			a1.a_value -= dot[segment];
			a1.a_value -= 2;
			outab(opcode);
			outrabrel(&a1);
		}
		break;
	case TIMM8:
		getaddr(&a1);
		constify(&a1);
		istuser(&a1);
		outab(opcode);
		if (a1.a_value > 0xFF)
			aerr(RANGE);
		outab(a1.a_value);
		break;
	case TPUSH:
		getaddr_mem(&a1, &mod1);
		ta1 = a1.a_type & TMMODE;
		if ((a1.a_type & TMADDR) == TMODRM) {
			/* Only an address is permitted TODO: 186 const */
			if (mod1 != 0x06)
				aerr(BADMODE);
//			outsegment();
			outab(0xFF);
			outab(mod1 | 0x30);
			/* FIXME raw or relraw */
			outraw(&a1);
			break;
		} else {
			/* Must be a reg16 or seg */
			if (ta1 == TWR)
				outab(0x50 | (a1.a_type & TMREG));
			else if (ta1 == TSR)
				outab(0x06 | ((a1.a_type & TMREG) << 3));
			else
				aerr(BADMODE);
		}
		break;
	case TPOP:
		getaddr_mem(&a1, &mod1);
		ta1 = a1.a_type & TMMODE;
		if ((a1.a_type & TMADDR) == TMODRM) {
			/* Only an address is permitted */
			if (mod1 != 0x06)
				aerr(BADMODE);
//			outsegment();
			outab(0x8F);
			outab(mod1 | 0x30);
			/* FIXME raw or relraw */
			outraw(&a1);
			break;
		} else {
			/* Must be a reg16 or seg */
			if (ta1 == TWR)
				outab(0x58 | (a1.a_type & TMREG));
			else if (ta1 == TSR)
				outab(0x07 | ((a1.a_type & TMREG) << 3));
			else
				aerr(BADMODE);
		}
		break;
	case TXCHG:
		/* Would be almost regular except there is a short form for
		   AX,reg */
		getaddr_mem(&a1, &mod1);
		comma();
		getaddr_mem(&a2, &mod2);
		ta1 = a1.a_type & TMMODE;
		ta2 = a2.a_type & TMMODE;
		/* Short form using AX */
		if (a1.a_type == (TWR | AX) && ta2 == TWR) {
			outab(0x90 | (a2.a_type & TMREG));
			break;
		}
		/* Either way around is the same for XCHG */
		if (a2.a_type == (TWR | AX) && ta1 == TWR) {
			outab(0x90 | (a1.a_type & TMREG));
			break;
		} else if (ta1 == TWR && ta2 == TWR) {
			mod1 = 0xC0 | ((a1.a_type & TMREG) << 3) |
				(a2.a_type & TMREG);
			outab(0x87);
			outab(mod1);
			break;
		} else if (ta1 == TBR && ta2 == TBR) {
			mod1 = 0xC0 | ((a1.a_type & TMREG) << 3) |
				(a2.a_type & TMREG);
			outab(0x86);
			outab(mod1);
			break;
		} else if ((a1.a_type & TMADDR) == TMODRM) {
			/* addr, reg */
			if ((a2.a_type & TMMODE) == TWR) {
				mod1 |= (a2.a_type & TMREG) << 3;
//				outsegment();
				outab(0x87);
				/* TODO: block xchg with immediate and some  others like that */
				outmod(mod1, &a1);
				break;
			}
			if ((a2.a_type & TMMODE) == TBR) {
				mod1 |= (a2.a_type & TMREG) << 3;
//				outsegment();
				outab(0x86);
				/* TODO: block xchg with immediate and some  others like that */
				outmod(mod1, &a1);
				break;
			}
		} else if ((a2.a_type & TMADDR) == TMODRM) {
			/* reg, addr */
			if ((a1.a_type & TMMODE) == TWR) {
				mod2 |= (a1.a_type & TMREG) << 3;
//				outsegment();
				outab(0x87);
				/* TODO: block xchg with immediate and some  others like that */
				outmod(mod2, &a1);
				break;
			}
			if ((a1.a_type & TMMODE) == TBR) {
				mod2 |= (a1.a_type & TMREG) << 3;
//				outsegment();
				outab(0x86);
				/* TODO: block xchg with immediate and some  others like that */
				outmod(mod2, &a1);
				break;
			}
		}
		aerr(BADMODE);
		break;
	case TIO:
		getaddr_mem(&a1, &mod1);
		comma();
		getaddr_mem(&a2, &mod2);
		if (a1.a_type == (TWR|AX))
			opcode |= 1;
		else if (a1.a_type != (TBR|AL))
			aerr(BADMODE);
		if ((a2.a_type & TMADDR) == TIMMED) {
			constify(&a2);
			istuser(&a2);
			outab(opcode);
			outrab(&a2);
			break;
		} else if (a2.a_type == (TWR|DX)) {
			outab(opcode | 0x08);
			break;
		} else
			aerr(BADMODE);
		break;
	case TRM:
		/* inc, dec, neg, not */
		getaddr_mem(&a1, &mod1);
		ta1 = a1.a_type & TMMODE;
		/* INC and DEC short forms */
		if ((opcode >> 8) == 0xFE && ta1 == TWR) {
			opcode = 0x40 | (opcode & 8);
			outab(opcode | (a1.a_type & TMREG));
			break;
		}
		if (ta1 == TWR) {
			outab((opcode >> 8) | 1);
			outab(0xC0 | (a1.a_type & TMREG));
			break;
		}
		if (ta1 == TBR) {
			outab(opcode >> 8);
			outab(0xC0 | (a1.a_type & TMREG));
			break;
		}
		/* Long forms of all */
		if ((a1.a_type & TMADDR) == TMODRM) {
//			outsegment();
			outab((opcode >> 8) | opsize(mod1, &a1));
			mod1 |= opcode & 0xFF;
			outmod(mod1, &a1);
			break;
		}
		aerr(BADMODE);
		break;
	default:
		aerr(SYNTAX_ERROR);
	}
	goto loop;
}
