	.export __shl

;	TOS << EA

__shl:
	and a,=15
	bz nowork

	st a,:__tmp
	pop p2
	pop ea
	push p2

	ld t,ea
loop:
	ld ea,t
	sl ea
	ld t,ea
	dld a,:__tmp
	bnz loop

	ld ea,t
	ret

nowork:
	pop p2
	pop ea
	push p2
	ret