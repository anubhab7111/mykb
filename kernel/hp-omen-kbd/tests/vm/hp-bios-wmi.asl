/*
 * The HP BIOS WMI interface as the VM tests see it: an SSDT with the GUID and
 * method the module calls, holding a color table and a backlight state in
 * memory. It checks the module's logic and how it sits in the kernel, not
 * whether the module is compatible with the real firmware; that is step 1 of
 * docs/hardware-validation.md.
 *
 * Each call must have the exact shape the firmware calls in the P3 plan list:
 * instance, method id, request size, data size. Anything else gets code 6, which
 * no test expects, so a call the module gets wrong fails the scenario.
 *
 * iasl -D picks a variant: KBD_TYPE_3, KBD_TYPE_0, KBD_RET_3, SHORT_GET,
 * SET_RET_5 or HEADER_DRIFT. tests/vm/run.sh loads the one a scenario names.
 */
#ifdef KBD_TYPE_3
#define KBD_TYPE 3
#endif
#ifdef KBD_TYPE_0
#define KBD_TYPE 0
#endif
#ifndef KBD_TYPE
#define KBD_TYPE 2
#endif

#ifdef KBD_RET_3
#define KBD_RET 3
#else
#define KBD_RET 0
#endif

#ifdef SET_RET_5
#define SET_RET 5
#else
#define SET_RET 0
#endif

DefinitionBlock ("", "SSDT", 2, "OMENKB", "HPBIOSEM", 1)
{
    Device (\_SB.HPEM)
    {
        Name (_HID, "PNP0C14")
        Name (_UID, "HPOMENEMU")

        /* One block: GUID 5FB7F034-2C63-45E9-BE91-3D44E2C707E4, method "WMAA". */
        Name (_WDG, Buffer (20)
        {
            0x34, 0xF0, 0xB7, 0x5F, 0x63, 0x2C, 0xE9, 0x45,
            0xBE, 0x91, 0x3D, 0x44, 0xE2, 0xC7, 0x07, 0xE4,
            0x41, 0x41,     /* object id "AA" */
            0x01,           /* instance count */
            0x02            /* flags: method */
        })

        Name (KTYP, KBD_TYPE)
        Name (KRET, KBD_RET)
        Name (SRET, SET_RET)
        Name (BKLT, 0xE4)

        Name (TABL, Buffer (128)
        {
            /* Bytes 0-24: the header nobody documents. */
            0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
            0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10,
            0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
            0x19,
            /* Bytes 25-36: 40E0D0 40E0D0 40E0D0 0099ED, the P1 baseline in hardware order. */
            0x40, 0xE0, 0xD0, 0x40, 0xE0, 0xD0, 0x40, 0xE0, 0xD0, 0x00, 0x99, 0xED,
            /* Bytes 37-127: a pattern a write must not disturb. */
            0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5,
            0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5,
            0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5,
            0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5,
            0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5,
            0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5,
            0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5
        })

        /* A reply without data: u32 sigpass 0, then u32 return code. */
        Method (RCOD, 1, Serialized)
        {
            Local0 = Buffer (8) {}
            CreateDWordField (Local0, 4, CODE)
            CODE = Arg0
            Return (Local0)
        }

        /* A reply with data. */
        Method (RDAT, 2, NotSerialized)
        {
            Return (Concatenate (RCOD (Arg0), Arg1))
        }

        /* The kernel calls this as WMAA (instance, method id, 144-byte request). */
        Method (WMAA, 3, Serialized)
        {
            CreateDWordField (Arg2, 0, SIGN)
            CreateDWordField (Arg2, 4, COMD)
            CreateDWordField (Arg2, 8, QERY)
            CreateDWordField (Arg2, 12, DSIZ)
            CreateByteField (Arg2, 16, DAT0)

            If (SIGN != 0x55434553)
            {
                Return (RCOD (2))
            }

            If ((Arg0 != 0) || (SizeOf (Arg2) != 144))
            {
                Return (RCOD (6))
            }

            If ((COMD == 0x00020008) && (QERY == 0x2B))
            {
                If ((Arg1 != 2) || (DSIZ != 1))
                {
                    Return (RCOD (6))
                }

                Return (RDAT (KRET, Mid (ToBuffer (KTYP), 0, 1)))
            }

            If (COMD != 0x00020009)
            {
                Return (RCOD (3))
            }

            If (QERY == 0x02)
            {
                If ((Arg1 != 3) || (DSIZ != 128))
                {
                    Return (RCOD (6))
                }

#ifdef SHORT_GET
                Return (Buffer (4) {})
#else
                Return (RDAT (0, TABL))
#endif
            }

            If (QERY == 0x03)
            {
                If ((Arg1 != 3) || (DSIZ != 128))
                {
                    Return (RCOD (6))
                }

                If (SRET == 0)
                {
                    TABL = Mid (Arg2, 16, 128)
#ifdef HEADER_DRIFT
                    TABL [0] = DerefOf (TABL [0]) + 1
#endif
                }

                Return (RDAT (SRET, TABL))
            }

            If (QERY == 0x04)
            {
                If ((Arg1 != 2) || (DSIZ != 4))
                {
                    Return (RCOD (6))
                }

                Return (RDAT (0, Mid (ToBuffer (BKLT), 0, 4)))
            }

            If (QERY == 0x05)
            {
                If ((Arg1 != 2) || (DSIZ != 1) || ((DAT0 != 0xE4) && (DAT0 != 0x64)))
                {
                    Return (RCOD (6))
                }

                BKLT = DAT0
                Return (RDAT (0, Mid (ToBuffer (BKLT), 0, 1)))
            }

            Return (RCOD (3))
        }
    }
}
