; vdfs.asm
;
; VDFS for B-Em
; Copyright 2018 Steve Fosdick.
;
; This module implements the ROM part of a Virtual Disk Filing
; System, one in which a part of filing system of the host is
; exposed to the guest through normal OS calls on the guest.
;
; This particular implementation comes in two parts:
;
; 1. This ROM which runs on the guest, the emulated BBC.  This
;    forwards ROM service calls to the module running in the host
;    and, when selected as the current filing system, takes over
;    the filing system vectors.  Calls made to the filing system
;    vectors are also forwarded to the host module.  Under control
;    of the host module this ROM then performs certain operations
;    with the BBC MOS.
;
; 2. The vdfs.c module which runs as part of the emultor, on the host.
;
; This ROM passes control to the host module by writing to a pair
; of ports in the expansion area in FRED.  Two further ports are
; used to communicate a small set of flags and the number of the
; filing system VDFS was selected as.
;
; The host module returns control to this ROM by jumping to an
; address in a dispatch table whose size and address are stored
; at the very beginning of the ROM where the language entry point
; would be if this were a language ROM.

fsno_vdfs   =   &11                 ; VDFS normal filing system no.
fsno_dfs    =   &04                 ; DFS filing system no.
fsno_adfs   =   &08                 ; ADFS filing system no.

; Values for the flags in port_flags, below

claim_adfs  =   &80
claim_dfs   =   &40

; The interface between this ROM and the vdfs.c module, four ports
; in the FRED 1Mhz bus area.

port_flags  =   &FC5C               ; various flags.
port_fsid   =   &FC5D               ; filing system ID.
port_cmd    =   &FC5E               ; execute actions on host.
port_a      =   &FC5F               ; store A ready for command.

; OS entry points.

OSCLI       =   &FFF7
OSBYTE      =   &FFF4
OSWORD      =   &FFF1
OSWRCH      =   &FFEE
OSNEWL      =   &FFE7
OSASCI      =   &FFE3
OSFILE      =   &FFDD
OSARGS      =   &FFDA
OSBGET      =   &FFD7
OSBPUT      =   &FFD4
OSGBPB      =   &FFD1
OSFIND      =   &FFCE
OSRDRM      =   &FFB9

; OS vectors.

BRKV        =   &0202
EVNTV       =   &0220

; Zero page workspace.

romtab      =   &A8
romid       =   &AA
copywr      =   &AB

dmpadd      =   &A8
dmpcnt      =   &AB

ltflag      =   &A8
ltpchr      =   &A9
lineno      =   &AA

; Standard BBC Micro service ROM header except that what would
; be the language entry point in a language ROM contains details
; of the dispatch table the vdfs.c module uses to transfer control
; top this ROM.

            org     &8000
.start      equb    (dispend-disptab)/2 ; no. entires in table.
            equw    disptab
            jmp     service
            equb    &82                 ; ROM type.
            equb    copyright-start
.romversion equb    &06
.romtitle   equs    "B-Em VDFS", &00
            include "version.asm"
.copyright  equb    &00
            equs    "(C) 2018 Steve Fosdick, GPL3", &00
            equd    0
.banner     equs    "Virtual DFS", &00

; The dispatch table.  This needs to be in the same order as
; enum vdfs_action in the vdfs.c module.

.disptab    equw    serv_done       ; all done.
            equw    fsstart         ; normal filing system start.
            equw    fsboot          ; filing system start at boot.
            equw    fs_info         ; give OS filing system info.
            equw    dir_cat         ; *CAT  via OSFSC
            equw    dir_ex          ; *EX   via OSFSC
            equw    pr_all          ; *INFO via ISFSC
            equw    cmd_dump        ; *DUMP
            equw    cmd_list        ; *LIST
            equw    cmd_print       ; *PRiNT
            equw    cmd_type        ; *TYPE
            equw    cmd_roms        ; *ROMS
            equw    tube_exec       ; start execution ob tube proc.
            equw    tube_init       ; initialise tube.
            equw    tube_explode    ; explode character set for tube.
.dispend

; Stubs to transfer control to the vdfs.c module.

.service    sta     port_a
            lda     #&00
            sta     port_cmd
.serv_done  rts
.file       sta     port_a
            lda     #&01
            sta     port_cmd
            rts
.args       sta     port_a
            lda     #&02
            sta     port_cmd
            rts
.bget       sta     port_a
            lda     #&03
            sta     port_cmd
            rts
.bput       sta     port_a
            lda     #&04
            sta     port_cmd
            rts
.gbpb       sta     port_a
            lda     #&05
            sta     port_cmd
            rts
.find       sta     port_a
            lda     #&06
            sta     port_cmd
            rts
.fsc        sta     port_a
            lda     #&07
            sta     port_cmd
            rts

; Filing system startup.  This is called directly when the filing
; system was selected by ROM service call &12 or by OS command.

.fsstart
{
            tya
            pha
            lda     #&06            ; Inform current FS new FS taking over
            jsr     callfscv
            ldx     #&00
            ldy     #&1B            ; Set up new vectors to point
.vecloop    tya                     ; into the extended vector area
            sta     &212,x          ; at &FFxx
            lda     #&FF
            sta     &213,x
            lda     vectab,x        ; Set the extended vector to the
            inx                     ; address in this ROM.
            sta     &0d9f,y
            iny
            lda     vectab,x
            inx
            sta     &0d9f,y
            iny
            lda     &F4             ; and include our ROM number in
            sta     &0d9f,y         ; the extended vector.
            iny
            cpx     #&0e
            bne     vecloop
            lda     #&8f
            ldx     #&0f
            jsr     OSBYTE          ; Notify that vectors have changed
            pla
            tay
            rts
.callfscv   jmp     (&021E)
.vectab     equw    file
            equw    args
            equw    bget
            equw    bput
            equw    gbpb
            equw    find
            equw    fsc
}

.prtitle
{
            ldx     #&00
.loop       lda     banner,x
            beq     done
            jsr     OSWRCH
            inx
            bne     loop
.done       rts
}

; Filing system boot.  This is called in response to ROM service
; call &03 when the code in vdfs.c has determined that VDFS is the
; filing system selected.

.fsboot
{
            tya                     ; save the boot flag.
            pha
            jsr     prtitle         ; announce the filing system
            jsr     OSNEWL
            jsr     OSNEWL
            jsr     fsstart         ; same setup as for call &12.
            pla
            bne     noboot          ; then maybe exec !BOOT.
            lda     #&40
            ldx     #<name
            ldy     #>name
            jsr     OSFIND
            cmp     #&00
            bne     found
            rts
.found      tax                     ; Found a !BOOT file.
            ldy     #&00            ; Set as the current EXEC file.
            lda     #&C6
            jsr     OSBYTE
.noboot     lda     #&00
            rts
.name       equs    "!BOOT",&0d
}

; Filing system info.  This is in response to ROM service call
; &25 which is master-specific.  This is where we tell the OS
; which filing system names, numbers and ranges of file handles
; we can respond to.  Which filing system entries we give back
; depends on what filing systems we are claiming to be.

.fs_info
{
            ldx     #&00            ; always copy the VDFS entry.
.loopv      lda     vdfs_ent,x
            sta     (&f2),y
            iny
            inx
            cpx     #&0b
            bne     loopv
            bit     port_flags      ; only copy ADFS if we're
            bpl     noadfs          ; claiming to be ADFS.
            ldx     #&00
.loopa      lda     adfs_ent,x
            sta     (&f2),y
            iny
            inx
            cpx     #&0b
            bne     loopa
            bit     port_flags      ; only copy DFS (DISC/DISK) if
.noadfs     bvc     nodfs           ; we're claiming to be DFS.
            ldx     #&00
.loopd      lda     dfs_ents,x
            sta     (&f2),y
            iny
            inx
            cpx     #&16
            bne     loopd
.nodfs      lda     #&25
            rts
.vdfs_ent   equs    "VDFS    ", &80, &FF, &11
.adfs_ent   equs    "ADFS    ", &30, &3A, &08
.dfs_ents   equs    "DISK    ", &11, &15, &04
            equs    "DISC    ", &11, &15, &04
}

; Routines to list information about files.

            macro   outcnt char
            lda     #char
            jsr     OSWRCH
            inx
            endmacro

            macro   pr_attr mask, char
            lda     #mask
            bit     &010a
            beq     notset
            outcnt  char
.notset
            endmacro

.pr_basic
{
            ldx     #&00            ; print characters of the name.
.loop       lda     &0100,x
            jsr     OSWRCH
            inx
            cpx     #&0a
            bne     loop
            outcnt  ' '
            jsr     OSWRCH
            inx
            bit     &010b           ; test most significant byte of
            bvc     notdir          ; attributes for the directory flag.
            outcnt  'D'
.notdir     pr_attr &08, 'L'
            pr_attr &02, 'W'
            pr_attr &01, 'R'
            outcnt  '/'
            pr_attr &20, 'w'
            pr_attr &10, 'r'
            cpx     #&14
            bcs     done
            lda     #' '
.spcloop    jsr     OSWRCH
            inx
            cpx     #&14
            bne     spcloop
.done       rts
}

.hexbyt     pha
            lsr     A
            lsr     A
            lsr     A
            lsr     A
            jsr     hexnyb
            pla
            and     #&0f
.hexnyb     ora     #'0'
            cmp     #'9'+1
            bcc     ddig
            adc     #&06
.ddig       jmp     OSWRCH

            macro   hexout addr
            lda     addr
            jsr     hexbyt
            endmacro

            macro   twospc
            lda     #' '
            jsr     OSWRCH
            jsr     OSWRCH
            endmacro

.pr_all     jsr     pr_basic
            twospc
            hexout  &010f
            hexout  &010e
            hexout  &010d
            hexout  &010c
            twospc
            hexout  &0113
            hexout  &0112
            hexout  &0111
            hexout  &0110
            twospc
            hexout  &0117
            hexout  &0116
            hexout  &0115
            hexout  &0114
            jmp     OSNEWL

.cat_loop   jsr     pr_basic
.dir_cat    lda     #&08
            sta     port_cmd
            bcc     cat_loop
            jmp     OSNEWL

.ex_loop    jsr     pr_all
.dir_ex     lda     #&08
            sta     port_cmd
            bcc     ex_loop
            rts

.file_info  rts

.not_found
{
            ldx     #end-msg
.loop       lda     msg,x
            sta     &0100,x
            dex
            bpl     loop
            jmp     &0100
.msg        brk
            equs    "Not found"
            equb    &00
.end
}

; The *DUMP command.

.cmd_dump
{
            lda     #&40
            jsr     OSFIND
            tay
            beq     not_found
            pha
            lda     #&87            ; find screen mode.
            jsr     OSBYTE
            lda     #&08
            cpy     #&00
            beq     wide
            cpy     #&03
            bne     narrow
.wide       asl     a
.narrow     sta     dmpcnt
            pla
            tay
            lda     #&00
            sta     dmpadd
            sta     dmpadd+1
            sta     dmpadd+2
            bit     &FF
            bmi     gotesc
.linlp      lda     dmpadd+2
            jsr     hexbyt
            lda     dmpadd+1
            jsr     hexbyt
            lda     dmpadd
            jsr     hexbyt
            lda     #' '
            jsr     OSWRCH
            ldx     dmpcnt
.getlp      jsr     OSBGET
            bcs     skip
            sta     &0100,X
            jsr     hexbyt
            lda     #' '
            jsr     OSWRCH
            dex
            bne     getlp
            clc
.skip       php
            bcc     ascii
.endlp      lda     #'*'
            jsr     OSWRCH
            jsr     OSWRCH
            lda     #' '
            jsr     OSWRCH
            lda     #&00
            sta     &0100,X
            dex
            bne     endlp
.ascii      ldx     dmpcnt
.asclp      lda     &0100,X
            and     #&7F
            cmp     #&7F
            beq     nonprt
            cmp     #&20
            bcs     print
.nonprt     lda     #'.'
.print      jsr     OSWRCH
            dex
            bne     asclp
            jsr     OSNEWL
            plp
            bcs     eof
            lda     dmpcnt
            clc
            adc     dmpadd
            sta     dmpadd
            lda     #&00
            adc     dmpadd+1
            sta     dmpadd+1
            bcc     noinc
            inc     dmpadd+2
.noinc      bit     &FF
            bpl     linlp
.gotesc     lda     #&7E
            jsr     OSWRCH
.eof        lda     #&00
            jmp     OSFIND
}

; Useful subroutines.

.bcdbyt     pha
            php
            lsr     A
            lsr     A
            lsr     A
            lsr     A
            plp
            jsr     bcdnyb
            pla
.bcdnyb     and     #&0f
            bne     bcddig
            bcc     bcddig
            lda     #' '
            jsr     OSWRCH
            sec
            rts
.bcddig     ora     #'0'
            jsr     OSWRCH
            clc
            rts

.outesc
{
            tax
            bmi     high
.high2      cmp     #' '
            bcc     low
            inx
            bmi     del
            cmp     #'|'
            bne     notbar
            jsr     OSWRCH
.notbar     jmp     OSWRCH
.high       lda     #'|'
            jsr     OSWRCH
            lda     #'!'
            jsr     OSWRCH
            txa
            and     #&7f
            tax
            jmp     high2
.low        ora     #&40
            tax
            lda     #'|'
            jsr     OSWRCH
            txa
            jmp     OSWRCH
.del        lda     #'|'
            jsr     OSWRCH
            lda     #'?'
            jmp     OSWRCH
}

; The *LIST and *TYPE commands.

.cmd_list   lda     #&00
            sta     lineno
            sta     lineno+1
            sta     lineno+2
            beq     lstype

.cmd_type   lda     #&80

.lstype
{
            sta     ltflag
            lda     #&40
            jsr     OSFIND
            tay
            bne     found
            jmp     not_found
.pline      tax
            sed
            sec
            lda     #&00
            adc     lineno
            sta     lineno
            lda     #&00
            adc     lineno+1
            sta     lineno+1
            cld
            sec
            jsr     bcdbyt
            lda     lineno
            php
            lsr     A
            lsr     A
            lsr     A
            lsr     A
            plp
            jsr     bcdnyb
            lda     lineno
            clc
            jsr     bcdnyb
            lda     #' '
            jsr     OSWRCH
            txa
.chrlp      cmp     #&0D
            beq     newlin
            cmp     #&0A
            beq     newlin
            sta     ltpchr
            jsr     outesc
.rdchr      jsr     OSBGET
            bcc     chrlp
.eof        jsr     OSNEWL
            lda     #&00
            jmp     OSFIND
.newlin     cmp     ltpchr
            beq     blalin
            pha
            lda     ltpchr
            cmp     #&0D
            beq     nl2nd
            cmp     #&0A
            beq     nl2nd
            pla
            sta     ltpchr
.blalin     jsr     OSNEWL
.found      bit     &FF
            bmi     gotesc
            jsr     OSBGET
            bcs     eof
            bit     ltflag
            bmi     chrlp
            bpl     pline
.nl2nd      lda     #&00
            sta     ltpchr
            pla
            jmp     rdchr
.gotesc     lda     #&7E
            jsr     OSBYTE
            lda     #&00
            jmp     OSFIND
}

.cmd_print
{
            lda     #&40
            jsr     OSFIND
            tay
            bne     found
            jmp     not_found
.chrlp      jsr     OSWRCH
.found      jsr     OSBGET
            bcs     eof
            bit     &FF
            bpl     chrlp
            lda     #&7E
            jsr     OSBYTE
.eof        lda     #&00
            jmp     OSFIND
}

; *ROMS

.cmd_roms
{
            lda     #&aa
            ldx     #&00
            ldy     #&ff
            jsr     OSBYTE
            stx     romtab
            sty     romtab+1
            jsr     OSNEWL
            ldy     #&0f
.rmloop     sty     romid
            lda     #&09
            sta     port_cmd
            bcs     gotram
            lda     (romtab),y
            bne     gotrom
.next       dey
            bpl     rmloop
            jsr     OSNEWL
            lda     #&00
            rts
.gotrom     tax
            jsr     prinfo
            jsr     space
            jsr     cparen
            jsr     rdcpyr
            jsr     prtitl
            ldy     romid
            jmp     next
.gotram     jsr     rdcpyr
            sta     &f6
            ldy     romid
            jsr     OSRDRM
            cmp     #&00
            bne     empty
            inc     &f6
            ldy     romid
            jsr     OSRDRM
            cmp     #'('
            bne     empty
            inc     &f6
            ldy     romid
            jsr     OSRDRM
            cmp     #'C'
            bne     empty
            inc     &f6
            ldy     romid
            jsr     OSRDRM
            cmp     #')'
            bne     empty
            lda     #&06
            sta     &f6
            ldy     romid
            jsr     OSRDRM
            tax
            jsr     prinfo
            jsr     rparen
            jsr     prtitl
            ldy     romid
            jmp     next
.empty      ldx     #&00
            jsr     prinfo
            jsr     rparen
            jsr     OSNEWL
            ldy     romid
            jmp     next

.rdcpyr     lda     #&07
            sta     &f6
            lda     #&80
            sta     &f7
            ldy     romid
            jsr     OSRDRM
            sta     copywr
            rts

.prinfo     lda     #'R'
            jsr     OSWRCH
            lda     #'o'
            jsr     OSWRCH
            lda     #'m'
            jsr     OSWRCH
            jsr     space
            lda     romid
            cmp     #&0A
            bcs     geten
            lda     #'0'
            jsr     OSWRCH
            lda     romid
            jmp     both
.geten      lda     #'1'
            jsr     OSWRCH
            lda     romid
            sec
            sbc     #&0a
.both       and     #&0f
            clc
            adc     #'0'
            jsr     OSWRCH
            jsr     space
            lda     #':'
            jsr     OSWRCH
            jsr     space
            lda     #'('
            jsr     OSWRCH
            txa
            and     #&80
            beq     notsrv
            lda     #'S'
            bne     issrv
.notsrv     lda     #' '
.issrv      jsr     OSWRCH
            txa
            and     #&40
            beq     space
            lda     #'L'
            bne     islng
.space      lda     #' '
.islng      jmp     OSWRCH

.prtitl     jsr     space
            lda     #&09
            sta     &f6
            lda     #&80
            sta     &f7
.tloop      ldy     romid
            jsr     OSRDRM
            cmp     #&7f
            bcs     cchar
            cmp     #' '
            bcs     pchar
            cmp     #&00
            bne     cchar
            lda     #' '
.pchar      jsr     OSWRCH
.cchar      inc     &f6
            lda     &f6
            cmp     copywr
            bne     tloop
            jmp     OSNEWL

.romchk     lda     copywr
            sta     &f6
            ldy     romid
            jsr     OSRDRM
            cmp     #&00
            bne     chkfai
            inc     &f6
            ldy     romid
            jsr     OSRDRM
            cmp     #'('
            bne     chkfai
            inc     &f6
            ldy     romid
            jsr     OSRDRM
            cmp     #'C'
            bne     chkfai
            inc     &f6
            ldy     romid
            jsr     OSRDRM
            cmp     #')'
            bne     chkfai
            clc
            rts
.chkfai     sec
            rts
.copyst     equs    ")C("
            equb    &00

.rparen     lda     #'R'
            jsr     OSWRCH
.cparen     lda     #')'
            jmp     OSWRCH
}

; Start executation in the tube.  This will be called at the tail
; or a */, *RUN or when an unrecognised command is satified by a
; file on disk and the code concerned needs to execute over the tube.

.tube_exec  lda     #&D1            ; claim the tube.
            jsr     &0406
            bcc     tube_exec
            lda     #&04            ; start executation at the 32 bit
            ldx     #&c0            ; address in &c0 which is set by
            ldy     #&00            ; code in vdfs.c
            jmp     &0406

            include "tubehost.asm"

.tube_init
{
            tya
            pha
            lda     #<TubeEvHnd     ; point EVENTV to tube host
            sta     EVNTV
            lda     #>TubeEvHnd
            sta     EVNTV+1
            lda     #<TubeBrkHnd    ; point BRKV to tube host
            sta     BRKV
            lda     #>TubeBrkHnd
            sta     BRKV+1
            lda     #&8E
            sta     &FEE0
            ldy     #&00            ; copy the tube host code into low memory
.copy1      lda     TubeHost1,Y
            sta     &0400,Y
            lda     TubeHost2,Y
            sta     &0500,Y
            lda     TubeHost3,Y
            sta     &0600,Y
            dey
            bne     copy1
            jsr     TubeCall
            ldx     #TubeBrkLen
.copyz      lda     TubeHostZ,X
            sta     TubeBrkHnd,X
            dex
            bpl     copyz
            pla
            tay
            lda     #&00
            rts
}

.tube_explode
{
            cpy     #&00
            beq     notube          ; if no tube.
            lda     #&14            ; explode character set.
            ldx     #&06
            jsr     OSBYTE
.imsglp     bit     &FEE0           ; wait for character to be send from tube
            bpl     imsglp
            lda     &FEE1           ; fetch the character.
            beq     done            ; end of message?
            jsr     OSWRCH
            jmp     imsglp
.done       lda     #&ff
.notube     rts
}

.end
            save    "vdfs6", start, end
