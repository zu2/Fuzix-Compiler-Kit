	.export f__mull

f__mull:
	sta	3,__tmp,0
	lda	3,N32,1
	sta	3,__tmp2,0
	lda	2,__sp,0
	lda	3,0,2
	sta	3,__tmp3,0	; Low word
	lda	3,-1,2
	sta	3,__tmp4,0	; High word

	dsz	__sp,0
	dsz	__sp,0

	sub	2,2		; Clear result
	sub	3,3
	lda	0,__hireg,0

loop:
	movzl	2,2		; result low left
	movl	3,3		; result high left
	movzl	1,1		; input low left
	movl	0,0,snc		; input high left, skip if doing an add
	jmp	noadd,1
	;	Add the other input value (tmp3/4) to result
	sta	0,__tmp5,0
	sta	1,__tmp6,0
	lda	0,__tmp3,0
	lda	1,__tmp4,0
	addz	0,2,szc
	inc	1,1
	add	1,3
	lda	0,__tmp5,0
	lda	1,__tmp6,0
noadd:
	dsz	__tmp2		; count our 32 steps
	jmp	loop,1
	; Done.. result is in 2/3
	sta	3,__hireg,0
	mov	2,1
	lda	3,__fp,0
	jmp	@__tmp,0

N32:
	.word	32
