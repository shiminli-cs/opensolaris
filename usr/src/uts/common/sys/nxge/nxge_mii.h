/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2006 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#ifndef _SYS_NXGE_NXGE_MII_H_
#define	_SYS_NXGE_NXGE_MII_H_

#pragma ident	"%Z%%M%	%I%	%E% SMI"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Configuration Register space.
 */

#define	MII_BMCR		0
#define	MII_BMSR		1
#define	MII_IDR1		2
#define	MII_IDR2		3
#define	MII_ANAR		4
#define	MII_ANLPAR		5
#define	MII_ANER		6
#define	MII_NPTXR		7
#define	MII_LPRXNPR		8
#define	MII_GCR			9
#define	MII_GSR			10
#define	MII_RES0		11
#define	MII_RES1		12
#define	MII_RES2		13
#define	MII_RES3		14
#define	MII_ESR			15

#define	NXGE_MAX_MII_REGS	32

/*
 * Configuration Register space.
 */
typedef struct _mii_regs {
	uchar_t bmcr;		/* Basic mode control register */
	uchar_t bmsr;		/* Basic mode status register */
	uchar_t idr1;		/* Phy identifier register 1 */
	uchar_t idr2;		/* Phy identifier register 2 */
	uchar_t anar;		/* Auto-Negotiation advertisement register */
	uchar_t anlpar;		/* Auto-Negotiation link Partner ability reg */
	uchar_t aner;		/* Auto-Negotiation expansion register */
	uchar_t nptxr;		/* Next page transmit register */
	uchar_t lprxnpr;	/* Link partner received next page register */
	uchar_t gcr;		/* Gigabit basic mode control register. */
	uchar_t gsr;		/* Gigabit basic mode status register */
	uchar_t mii_res1[4];	/* For future use by MII working group */
	uchar_t esr;		/* Extended status register. */
	uchar_t vendor_res[16];	/* For future use by Phy Vendors */
} mii_regs_t, *p_mii_regs_t;

/*
 * MII Register 0: Basic mode control register.
 */
#define	BMCR_RES		0x003f  /* Unused... */
#define	BMCR_SSEL_MSB		0x0040  /* Used to manually select speed */
					/* (with * bit 6) when auto-neg */
					/* disabled */
#define	BMCR_COL_TEST		0x0080  /* Collision test */
#define	BMCR_DPLX_MD		0x0100  /* Full duplex */
#define	BMCR_RESTART_AN		0x0200  /* Auto negotiation restart */
#define	BMCR_ISOLATE		0x0400	/* Disconnect BCM5464R from MII */
#define	BMCR_PDOWN		0x0800	/* Powerdown the BCM5464R */
#define	BMCR_ANENABLE		0x1000	/* Enable auto negotiation */
#define	BMCR_SSEL_LSB		0x2000  /* Used to manually select speed */
					/* (with bit 13) when auto-neg */
					/* disabled */
#define	BMCR_LOOPBACK		0x4000	/* TXD loopback bits */
#define	BMCR_RESET		0x8000	/* Reset the BCM5464R */

typedef union _mii_bmcr {
	uint16_t value;
	struct {
#if defined(_BIT_FIELDS_HTOL)
		uint16_t reset:1;
		uint16_t loopback:1;
		uint16_t speed_sel:1;
		uint16_t enable_autoneg:1;
		uint16_t power_down:1;
		uint16_t isolate:1;
		uint16_t restart_autoneg:1;
		uint16_t duplex_mode:1;
		uint16_t col_test:1;
		uint16_t speed_1000_sel:1;
		uint16_t res1:6;
#elif defined(_BIT_FIELDS_LTOH)
		uint16_t res1:6;
		uint16_t speed_1000_sel:1;
		uint16_t col_test:1;
		uint16_t duplex_mode:1;
		uint16_t restart_autoneg:1;
		uint16_t isolate:1;
		uint16_t power_down:1;
		uint16_t enable_autoneg:1;
		uint16_t speed_sel:1;
		uint16_t loopback:1;
		uint16_t reset:1;
#endif
	} bits;
} mii_bmcr_t, *p_mii_bmcr_t;

/*
 * MII Register 1:  Basic mode status register.
 */
#define	BMSR_ERCAP		0x0001  /* Ext-reg capability */
#define	BMSR_JCD		0x0002  /* Jabber detected */
#define	BMSR_LSTATUS		0x0004  /* Link status */
#define	BMSR_ANEGCAPABLE	0x0008  /* Able to do auto-negotiation */
#define	BMSR_RFAULT		0x0010  /* Remote fault detected */
#define	BMSR_ANEGCOMPLETE	0x0020  /* Auto-negotiation complete */
#define	BMSR_MF_PRE_SUP		0x0040  /* Preamble for MIF frame suppressed, */
					/* always 1 for BCM5464R */
#define	BMSR_RESV		0x0080  /* Unused... */
#define	BMSR_ESTAT		0x0100  /* Contains IEEE extended status reg */
#define	BMSR_100BASE2HALF	0x0200  /* Can do 100mbps, 2k pkts half-dplx */
#define	BMSR_100BASE2FULL	0x0400  /* Can do 100mbps, 2k pkts full-dplx */
#define	BMSR_10HALF		0x0800  /* Can do 10mbps, half-duplex */
#define	BMSR_10FULL		0x1000  /* Can do 10mbps, full-duplex */
#define	BMSR_100HALF		0x2000  /* Can do 100mbps, half-duplex */
#define	BMSR_100FULL		0x4000  /* Can do 100mbps, full-duplex */
#define	BMSR_100BASE4		0x8000  /* Can do 100mbps, 4k packets */

typedef union _mii_bmsr {
	uint16_t value;
	struct {
#if defined(_BIT_FIELDS_HTOL)
		uint16_t link_100T4:1;
		uint16_t link_100fdx:1;
		uint16_t link_100hdx:1;
		uint16_t link_10fdx:1;
		uint16_t link_10hdx:1;
		uint16_t res2:2;
		uint16_t extend_status:1;
		uint16_t res1:1;
		uint16_t preamble_supress:1;
		uint16_t auto_neg_complete:1;
		uint16_t remote_fault:1;
		uint16_t auto_neg_able:1;
		uint16_t link_status:1;
		uint16_t jabber_detect:1;
		uint16_t ext_cap:1;
#elif defined(_BIT_FIELDS_LTOH)
		int16_t ext_cap:1;
		uint16_t jabber_detect:1;
		uint16_t link_status:1;
		uint16_t auto_neg_able:1;
		uint16_t remote_fault:1;
		uint16_t auto_neg_complete:1;
		uint16_t preamble_supress:1;
		uint16_t res1:1;
		uint16_t extend_status:1;
		uint16_t res2:2;
		uint16_t link_10hdx:1;
		uint16_t link_10fdx:1;
		uint16_t link_100hdx:1;
		uint16_t link_100fdx:1;
		uint16_t link_100T4:1;
#endif
	} bits;
} mii_bmsr_t, *p_mii_bmsr_t;

/*
 * MII Register 2: Physical Identifier 1.
 */
/* contains BCM OUI bits [3:18] */
typedef union _mii_idr1 {
	uint16_t value;
	struct {
		uint16_t ieee_address:16;
	} bits;
} mii_idr1_t, *p_mii_idr1_t;

/*
 * MII Register 3: Physical Identifier 2.
 */
typedef union _mii_idr2 {
	uint16_t value;
	struct {
#if defined(_BIT_FIELDS_HTOL)
		uint16_t ieee_address:6;
		uint16_t model_no:6;
		uint16_t rev_no:4;
#elif defined(_BIT_FIELDS_LTOH)
		uint16_t rev_no:4;
		uint16_t model_no:6;
		uint16_t ieee_address:6;
#endif
	} bits;
} mii_idr2_t, *p_mii_idr2_t;

/*
 * MII Register 4: Auto-negotiation advertisement register.
 */
#define	ADVERTISE_SLCT		0x001f  /* Selector bits for proto, 0x01 */
					/* indicates IEEE 802.3 CSMA/CD phy */
#define	ADVERTISE_CSMA		0x0001  /* Only selector supported */
#define	ADVERTISE_10HALF	0x0020  /* Try for 10mbps half-duplex  */
#define	ADVERTISE_10FULL	0x0040  /* Try for 10mbps full-duplex  */
#define	ADVERTISE_100HALF	0x0080  /* Try for 100mbps half-duplex */
#define	ADVERTISE_100FULL	0x0100  /* Try for 100mbps full-duplex */
#define	ADVERTISE_100BASE4	0x0200  /* Try for 100mbps 4k packets. set to */
					/* 0, BCM5464R not 100BASE-T4 capable */
#define	ADVERTISE_RES1		0x0400  /* Unused... */
#define	ADVERTISE_ASM_PAUS	0x0800  /* advertise asymmetric pause */
#define	ADVERTISE_PAUS		0x1000  /* can do full dplx pause */
#define	ADVERTISE_RFAULT	0x2000  /* Say we can detect faults */
#define	ADVERTISE_RES0		0x4000  /* Unused... */
#define	ADVERTISE_NPAGE		0x8000  /* Next page bit */

#define	ADVERTISE_FULL (ADVERTISE_100FULL | ADVERTISE_10FULL | \
			ADVERTISE_CSMA)
#define	ADVERTISE_ALL (ADVERTISE_10HALF | ADVERTISE_10FULL | \
			ADVERTISE_100HALF | ADVERTISE_100FULL)

typedef union _mii_anar {
	uint16_t value;
	struct {
#if defined(_BIT_FIELDS_HTOL)
		uint16_t np_indication:1;
		uint16_t acknowledge:1;
		uint16_t remote_fault:1;
		uint16_t res1:1;
		uint16_t cap_asmpause:1;
		uint16_t cap_pause:1;
		uint16_t cap_100T4:1;
		uint16_t cap_100fdx:1;
		uint16_t cap_100hdx:1;
		uint16_t cap_10fdx:1;
		uint16_t cap_10hdx:1;
		uint16_t selector:5;
#elif defined(_BIT_FIELDS_LTOH)
		uint16_t selector:5;
		uint16_t cap_10hdx:1;
		uint16_t cap_10fdx:1;
		uint16_t cap_100hdx:1;
		uint16_t cap_100fdx:1;
		uint16_t cap_100T4:1;
		uint16_t cap_pause:1;
		uint16_t cap_asmpause:1;
		uint16_t res1:1;
		uint16_t remote_fault:1;
		uint16_t acknowledge:1;
		uint16_t np_indication:1;
#endif
	} bits;
} mii_anar_t, *p_mii_anar_t;

/*
 * MII Register 5: Auto-negotiation link partner ability register.
 */
#define	LPA_SLCT		0x001f  /* Same as advertise selector */
#define	LPA_10HALF		0x0020  /* Can do 10mbps half-duplex */
#define	LPA_10FULL		0x0040  /* Can do 10mbps full-duplex */
#define	LPA_100HALF		0x0080  /* Can do 100mbps half-duplex */
#define	LPA_100FULL		0x0100  /* Can do 100mbps full-duplex */
#define	LPA_100BASE4		0x0200  /* Can do 100mbps 4k packets */
#define	LPA_RES1		0x0400  /* Unused... */
#define	LPA_ASM_PAUS		0x0800  /* advertise asymmetric pause */
#define	LPA__PAUS		0x1000  /* can do full dplx pause */
#define	LPA_RFAULT		0x2000	/* Link partner faulted */
#define	LPA_LPACK		0x4000	/* Link partner acked us */
#define	LPA_NPAGE		0x8000	/* Next page bit */

#define	LPA_DUPLEX		(LPA_10FULL | LPA_100FULL)
#define	LPA_100			(LPA_100FULL | LPA_100HALF | LPA_100BASE4)

typedef mii_anar_t mii_anlpar_t, *pmii_anlpar_t;

/*
 * MII Register 6: Auto-negotiation expansion register.
 */
#define	EXPANSION_LP_AN_ABLE	0x0001	/* Link partner has auto-neg cap */
#define	EXPANSION_PG_RX		0x0002	/* Got new RX page code word */
#define	EXPANSION_NP_ABLE	0x0004	/* This enables npage words */
#define	EXPANSION_LPNP_ABLE	0x0008	/* Link partner supports npage */
#define	EXPANSION_MFAULTS	0x0010	/* Multiple link faults detected */
#define	EXPANSION_RESV		0xffe0	/* Unused... */

typedef union _mii_aner {
	uint16_t value;
	struct {
#if defined(_BIT_FIELDS_HTOL)
		uint16_t res:11;
		uint16_t mlf:1;
		uint16_t lp_np_able:1;
		uint16_t np_able:1;
		uint16_t page_rx:1;
		uint16_t lp_an_able:1;
#else
		uint16_t lp_an_able:1;
		uint16_t page_rx:1;
		uint16_t np_able:1;
		uint16_t lp_np_able:1;
		uint16_t mlf:1;
		uint16_t res:11;
#endif
	} bits;
} mii_aner_t, *p_mii_aner_t;

/*
 * MII Register 7: Next page transmit register.
 */
typedef	union _mii_nptxr {
	uint16_t value;
	struct {
#if defined(_BIT_FIELDS_HTOL)
		uint16_t np:1;
		uint16_t res:1;
		uint16_t msgp:1;
		uint16_t ack2:1;
		uint16_t toggle:1;
		uint16_t res1:11;
#else
		uint16_t res1:11;
		uint16_t toggle:1;
		uint16_t ack2:1;
		uint16_t msgp:1;
		uint16_t res:1;
		uint16_t np:1;
#endif
	} bits;
} mii_nptxr_t, *p_mii_nptxr_t;

/*
 * MII Register 8: Link partner received next page register.
 */
typedef union _mii_lprxnpr {
	uint16_t value;
	struct {
#if defined(_BIT_FIELDS_HTOL)
		uint16_t np:1;
			uint16_t ack:1;
		uint16_t msgp:1;
		uint16_t ack2:1;
		uint16_t toggle:1;
		uint16_t mcf:11;
#else
		uint16_t mcf:11;
		uint16_t toggle:1;
		uint16_t ack2:1;
		uint16_t msgp:1;
		uint16_t ack:1;
		uint16_t np:1;
#endif
	} bits;
} mii_lprxnpr_t, *p_mii_lprxnpr_t;

/*
 * MII Register 9: 1000BaseT control register.
 */
typedef union _mii_gcr {
	uint16_t value;
	struct {
#if defined(_BIT_FIELDS_HTOL)
		uint16_t test_mode:3;
		uint16_t ms_mode_en:1;
		uint16_t master:1;
		uint16_t dte_or_repeater:1;
		uint16_t link_1000fdx:1;
		uint16_t link_1000hdx:1;
		uint16_t res:8;
#else
		uint16_t res:8;
		uint16_t link_1000hdx:1;
		uint16_t link_1000fdx:1;
		uint16_t dte_or_repeater:1;
		uint16_t master:1;
		uint16_t ms_mode_en:1;
		uint16_t test_mode:3;
#endif
	} bits;
} mii_gcr_t, *p_mii_gcr_t;

/*
 * MII Register 10: 1000BaseT status register.
 */
typedef union _mii_gsr {
	uint16_t value;
	struct {
#if defined(_BIT_FIELDS_HTOL)
		uint16_t ms_config_fault:1;
		uint16_t ms_resolve:1;
		uint16_t local_rx_status:1;
		uint16_t remote_rx_status:1;
		uint16_t link_1000fdx:1;
		uint16_t link_1000hdx:1;
		uint16_t res:2;
		uint16_t idle_err_cnt:8;
#else
		uint16_t idle_err_cnt:8;
		uint16_t res:2;
		uint16_t link_1000hdx:1;
		uint16_t link_1000fdx:1;
		uint16_t remote_rx_status:1;
		uint16_t local_rx_status:1;
		uint16_t ms_resolve:1;
		uint16_t ms_config_fault:1;
#endif
	} bits;
} mii_gsr_t, *p_mii_gsr_t;

/*
 * MII Register 15: Extended status register.
 */
typedef union _mii_esr {
	uint16_t value;
	struct {
#if defined(_BIT_FIELDS_HTOL)
		uint16_t link_1000Xfdx:1;
		uint16_t link_1000Xhdx:1;
		uint16_t link_1000fdx:1;
		uint16_t link_1000hdx:1;
		uint16_t res:12;
#else
			uint16_t res:12;
		uint16_t link_1000hdx:1;
		uint16_t link_1000fdx:1;
		uint16_t link_1000Xhdx:1;
		uint16_t link_1000Xfdx:1;
#endif
	} bits;
} mii_esr_t, *p_mii_esr_t;

#ifdef __cplusplus
}
#endif

#endif /* _SYS_NXGE_NXGE_MII_H_ */