#include "common.h"
#include "reg-alloc.h"
#include "latx-options.h"
#include "translate.h"

bool translate_sha1nexte(IR1_INST *pir1)
{
    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);
    IR2_OPND dst = ra_alloc_xmm(ir1_opnd_base_reg_num(opnd0));
    IR2_OPND src;
    IR2_OPND t0 = ra_alloc_ftemp();

    if (!ir1_opnd_is_mem(opnd1)) {
        src = ra_alloc_xmm(ir1_opnd_base_reg_num(opnd1));
    } else {
        src = ra_alloc_ftemp();
        assert(ir1_opnd_size(opnd1) == 128);
        load_freg128_from_ir1_mem(src, opnd1);
    }

    la_vrotri_w(t0, dst, 2);
    la_vadd_w(t0, t0, src);
    la_vor_v(dst, src, src);
    la_vextrins_w(dst, t0, VEXTRINS_IMM_4_0(3, 3));

    ra_free_temp(t0);
    ra_free_temp_auto(src);
    return true;
}

bool translate_sha1msg1(IR1_INST *pir1)
{
    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);
    IR2_OPND dst = ra_alloc_xmm(ir1_opnd_base_reg_num(opnd0));
    IR2_OPND src;
    IR2_OPND t0 = ra_alloc_ftemp();

    if (!ir1_opnd_is_mem(opnd1)) {
        src = ra_alloc_xmm(ir1_opnd_base_reg_num(opnd1));
    } else {
        src = ra_alloc_ftemp();
        assert(ir1_opnd_size(opnd1) == 128);
        load_freg128_from_ir1_mem(src, opnd1);
    }

    la_vor_v(t0, dst, dst);
    la_vpermi_w(t0, src, 0x4e);
    la_vxor_v(dst, dst, t0);

    ra_free_temp(t0);
    ra_free_temp_auto(src);
    return true;
}

bool translate_sha1msg2(IR1_INST *pir1)
{
    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);
    IR2_OPND dst = ra_alloc_xmm(ir1_opnd_base_reg_num(opnd0));
    IR2_OPND src;
    IR2_OPND t0 = ra_alloc_ftemp();
    IR2_OPND t1 = ra_alloc_ftemp();
    IR2_OPND x1 = ra_alloc_itemp();
    IR2_OPND x2 = ra_alloc_itemp();

    if (!ir1_opnd_is_mem(opnd1)) {
        src = ra_alloc_xmm(ir1_opnd_base_reg_num(opnd1));
    } else {
        src = ra_alloc_ftemp();
        assert(ir1_opnd_size(opnd1) == 128);
        load_freg128_from_ir1_mem(src, opnd1);
    }

    la_vbsll_v(t0, src, 4);
    la_vxor_v(t0, t0, dst);
    la_vrotri_w(t1, t0, 31);
    la_vpickve2gr_wu(x1, t1, 3);
    la_vpickve2gr_wu(x2, dst, 0);
    la_xor(x2, x2, x1);
    la_rotri_w(x2, x2, 31);
    la_vor_v(dst, t1, t1);
    la_vinsgr2vr_w(dst, x2, 0);

    ra_free_temp(x2);
    ra_free_temp(x1);
    ra_free_temp(t1);
    ra_free_temp(t0);
    ra_free_temp_auto(src);
    return true;
}

bool translate_sha1rnds4(IR1_INST *pir1)
{
    static const uint32_t ks[4] = {
        0x5A827999u, 0x6ED9EBA1u, 0x8F1BBCDCu, 0xCA62C1D6u
    };
    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);
    IR1_OPND *opnd2 = ir1_get_opnd(pir1, 2);
    IR2_OPND dst = ra_alloc_xmm(ir1_opnd_base_reg_num(opnd0));
    IR2_OPND src;
    IR2_OPND x1 = ra_alloc_itemp();
    IR2_OPND x2 = ra_alloc_itemp();
    IR2_OPND x3 = ra_alloc_itemp();
    IR2_OPND x4 = ra_alloc_itemp();
    IR2_OPND x5 = ra_alloc_itemp();
    IR2_OPND x6 = ra_alloc_itemp();
    IR2_OPND x7 = ra_alloc_itemp();
    int mode = ir1_opnd_uimm(opnd2) & 3;

    if (!ir1_opnd_is_mem(opnd1)) {
        src = ra_alloc_xmm(ir1_opnd_base_reg_num(opnd1));
    } else {
        src = ra_alloc_ftemp();
        assert(ir1_opnd_size(opnd1) == 128);
        load_freg128_from_ir1_mem(src, opnd1);
    }

    la_vpickve2gr_wu(x1, dst, 3);
    la_vpickve2gr_wu(x2, dst, 2);
    la_vpickve2gr_wu(x3, dst, 1);
    la_vpickve2gr_wu(x4, dst, 0);

    switch (mode) {
    case 0:
        la_rotri_w(x7, x1, 27);
        la_vpickve2gr_wu(x5, src, 3);
        la_add_w(x7, x7, x5);
        li_w(x5, ks[0]);
        la_add_w(x7, x7, x5);
        la_and(x6, x2, x3);
        la_andn(x5, x4, x2);
        la_xor(x5, x5, x6);
        la_add_w(x7, x7, x5);
        la_rotri_w(x5, x2, 2);

        la_rotri_w(x2, x7, 27);
        la_add_w(x2, x2, x4);
        la_vpickve2gr_wu(x4, src, 2);
        la_add_w(x2, x2, x4);
        li_w(x4, ks[0]);
        la_add_w(x2, x2, x4);
        la_and(x6, x1, x5);
        la_andn(x4, x3, x1);
        la_xor(x4, x4, x6);
        la_add_w(x2, x2, x4);
        la_rotri_w(x4, x1, 2);

        la_rotri_w(x1, x2, 27);
        la_add_w(x1, x1, x3);
        la_vpickve2gr_wu(x3, src, 1);
        la_add_w(x1, x1, x3);
        li_w(x3, ks[0]);
        la_add_w(x1, x1, x3);
        la_and(x6, x7, x4);
        la_andn(x3, x5, x7);
        la_xor(x3, x3, x6);
        la_add_w(x1, x1, x3);
        la_rotri_w(x3, x7, 2);

        la_rotri_w(x7, x1, 27);
        la_add_w(x7, x7, x5);
        la_vpickve2gr_wu(x5, src, 0);
        la_add_w(x7, x7, x5);
        li_w(x5, ks[0]);
        la_add_w(x7, x7, x5);
        la_and(x6, x2, x3);
        la_andn(x5, x4, x2);
        la_xor(x5, x5, x6);
        la_add_w(x7, x7, x5);
        la_rotri_w(x2, x2, 2);
        la_vxor_v(dst, dst, dst);
        la_vinsgr2vr_w(dst, x3, 0);
        la_vinsgr2vr_w(dst, x2, 1);
        la_vinsgr2vr_w(dst, x1, 2);
        la_vinsgr2vr_w(dst, x7, 3);
        break;
    case 1:
    case 3:
        li_w(x6, ks[mode]);

        la_rotri_w(x7, x1, 27);
        la_vpickve2gr_wu(x5, src, 3);
        la_add_w(x7, x7, x5);
        la_add_w(x7, x7, x6);
        la_xor(x5, x2, x3);
        la_xor(x5, x5, x4);
        la_add_w(x7, x7, x5);
        la_rotri_w(x5, x2, 2);

        la_rotri_w(x2, x7, 27);
        la_add_w(x2, x2, x4);
        la_vpickve2gr_wu(x4, src, 2);
        la_add_w(x2, x2, x4);
        la_add_w(x2, x2, x6);
        la_xor(x4, x1, x5);
        la_xor(x4, x4, x3);
        la_add_w(x2, x2, x4);
        la_rotri_w(x4, x1, 2);

        la_rotri_w(x1, x2, 27);
        la_add_w(x1, x1, x3);
        la_vpickve2gr_wu(x3, src, 1);
        la_add_w(x1, x1, x3);
        la_add_w(x1, x1, x6);
        la_xor(x3, x7, x4);
        la_xor(x3, x3, x5);
        la_add_w(x1, x1, x3);
        la_rotri_w(x3, x7, 2);

        la_rotri_w(x7, x1, 27);
        la_add_w(x7, x7, x5);
        la_vpickve2gr_wu(x5, src, 0);
        la_add_w(x7, x7, x5);
        la_add_w(x7, x7, x6);
        la_xor(x5, x2, x3);
        la_xor(x5, x5, x4);
        la_add_w(x7, x7, x5);
        la_rotri_w(x2, x2, 2);
        la_vxor_v(dst, dst, dst);
        la_vinsgr2vr_w(dst, x3, 0);
        la_vinsgr2vr_w(dst, x2, 1);
        la_vinsgr2vr_w(dst, x1, 2);
        la_vinsgr2vr_w(dst, x7, 3);
        break;
    default:
        li_w(x6, ks[2]);
        la_rotri_w(x7, x1, 27);
        la_vpickve2gr_wu(x5, src, 3);
        la_add_w(x7, x7, x5);
        la_add_w(x7, x7, x6);
        la_xor(x5, x2, x3);
        la_and(x6, x2, x3);
        la_and(x5, x5, x4);
        la_xor(x5, x5, x6);
        la_add_w(x7, x7, x5);
        la_rotri_w(x5, x2, 2);

        li_w(x6, ks[2]);
        la_rotri_w(x2, x7, 27);
        la_add_w(x2, x2, x4);
        la_vpickve2gr_wu(x4, src, 2);
        la_add_w(x2, x2, x4);
        la_add_w(x2, x2, x6);
        la_xor(x4, x1, x5);
        la_and(x6, x1, x5);
        la_and(x4, x4, x3);
        la_xor(x4, x4, x6);
        la_add_w(x2, x2, x4);
        la_rotri_w(x4, x1, 2);

        li_w(x6, ks[2]);
        la_rotri_w(x1, x2, 27);
        la_add_w(x1, x1, x3);
        la_vpickve2gr_wu(x3, src, 1);
        la_add_w(x1, x1, x3);
        la_add_w(x1, x1, x6);
        la_xor(x3, x7, x4);
        la_and(x6, x7, x4);
        la_and(x3, x3, x5);
        la_xor(x3, x3, x6);
        la_add_w(x1, x1, x3);
        la_rotri_w(x3, x7, 2);

        li_w(x6, ks[2]);
        la_rotri_w(x7, x1, 27);
        la_add_w(x7, x7, x5);
        la_vpickve2gr_wu(x5, src, 0);
        la_add_w(x7, x7, x5);
        la_add_w(x7, x7, x6);
        la_xor(x5, x2, x3);
        la_and(x6, x2, x3);
        la_and(x5, x5, x4);
        la_xor(x5, x5, x6);
        la_add_w(x7, x7, x5);
        la_rotri_w(x2, x2, 2);
        la_vxor_v(dst, dst, dst);
        la_vinsgr2vr_w(dst, x3, 0);
        la_vinsgr2vr_w(dst, x2, 1);
        la_vinsgr2vr_w(dst, x1, 2);
        la_vinsgr2vr_w(dst, x7, 3);
        break;
    }

    ra_free_temp(x7);
    ra_free_temp(x6);
    ra_free_temp(x5);
    ra_free_temp(x4);
    ra_free_temp(x3);
    ra_free_temp(x2);
    ra_free_temp(x1);
    ra_free_temp_auto(src);
    return true;
}

bool translate_sha256rnds2(IR1_INST *pir1)
{
    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);
    IR2_OPND dst = ra_alloc_xmm(ir1_opnd_base_reg_num(opnd0));
    IR2_OPND src;
    IR2_OPND xmm0 = ra_alloc_xmm(0);
    IR2_OPND x1 = ra_alloc_itemp();
    IR2_OPND x2 = ra_alloc_itemp();
    IR2_OPND x3 = ra_alloc_itemp();
    IR2_OPND x4 = ra_alloc_itemp();
    IR2_OPND x5 = ra_alloc_itemp();
    IR2_OPND x6 = ra_alloc_itemp();
    IR2_OPND x7 = ra_alloc_itemp();
    IR2_OPND vt0 = ra_alloc_ftemp();
    IR2_OPND vt2 = ra_alloc_ftemp();

    if (!ir1_opnd_is_mem(opnd1)) {
        src = ra_alloc_xmm(ir1_opnd_base_reg_num(opnd1));
    } else {
        src = ra_alloc_ftemp();
        assert(ir1_opnd_size(opnd1) == 128);
        load_freg128_from_ir1_mem(src, opnd1);
    }

    /* Raw x86 layout:
     * src = [f, e, b, a]
     * dst = [h, g, d, c]
     */
    la_vpickve2gr_wu(x1, src, 3); /* a0 */
    la_vpickve2gr_wu(x2, src, 2); /* b0 */
    la_vpickve2gr_wu(x3, dst, 3); /* c0 */
    la_rotri_w(x5, x1, 2);
    la_rotri_w(x6, x1, 13);
    la_xor(x5, x5, x6);
    la_rotri_w(x6, x1, 22);
    la_xor(x5, x5, x6);
    la_or(x7, x1, x2);
    la_and(x7, x7, x3);
    la_and(x6, x1, x2);
    la_or(x7, x7, x6);
    la_add_w(x7, x7, x5); /* maj + sigma0 */

    /* Round 1 T1 on the raw x86 layout. */
    la_vshuf4i_w(vt0, src, 0x00);
    la_vbitsel_v(vt0, dst, vt0, src);

    la_vpickve2gr_wu(x4, src, 1); /* e0 */
    la_vadd_w(vt2, xmm0, dst);    /* lane0 = msg0+h0, lane1 = msg1+h1 */
    la_rotri_w(x6, x4, 6);
    la_rotri_w(x5, x4, 11);
    la_xor(x6, x6, x5);
    la_rotri_w(x5, x4, 25);
    la_xor(x6, x6, x5);
    la_vshuf4i_w(vt0, vt0, 0x55); /* broadcast ch0 from lane1 */
    la_vadd_w(vt0, vt0, vt2);     /* lane0 = ch0 + msg0+h0 */
    la_vpickve2gr_wu(x5, vt0, 0);
    la_add_w(x6, x6, x5);         /* t1_0 */

    la_vpickve2gr_wu(x5, dst, 2); /* d0 */
    la_add_w(x7, x7, x6);
    la_add_w(x5, x5, x6);

    /* Start building the final x86-layout result early:
     * dst = [f2, e2, b2, a2], with f2=e1 and b2=a1 already known here.
     */
    la_vinsgr2vr_w(dst, x5, 0);
    la_vinsgr2vr_w(dst, x7, 2);

    /* sigma1(e1), started as early as possible. */
    la_rotri_w(x4, x5, 6);
    la_rotri_w(x6, x5, 11);
    la_xor(x4, x4, x6);
    la_rotri_w(x6, x5, 25);
    la_xor(x4, x4, x6);

    /* Round 2 reuses the scalar state from round 1:
     * dst.lane0 already holds e1, while src.lane1/src.lane0 hold f1/g1.
     */
    la_vshuf4i_w(vt0, src, 0x55); /* e0 broadcast = f1 */
    la_vbitsel_v(vt0, src, vt0, dst);
    /* a1 = x7, b1 = a0(x1), c1 = b0(x2), d1 = c0(x3). */
    la_rotri_w(x6, x7, 2);
    la_rotri_w(x5, x7, 13);
    la_xor(x6, x6, x5);
    la_rotri_w(x5, x7, 22);
    la_xor(x6, x6, x5);
    la_or(x5, x7, x1);
    la_and(x5, x5, x2);
    la_and(x7, x7, x1);
    la_or(x5, x5, x7);
    la_add_w(x6, x6, x5);

    la_vpickve2gr_wu(x5, vt2, 1); /* msg1+h1 */
    la_vpickve2gr_wu(x7, vt0, 0); /* ch(e1,f1,g1) */
    la_add_w(x6, x6, x5);         /* t2_partial + msg1+h1 */
    la_add_w(x3, x3, x5);         /* c0 + msg1+h1 */
    la_add_w(x4, x4, x7);         /* sigma1 + ch1 */
    la_add_w(x6, x6, x4);         /* new a2 */
    la_add_w(x4, x3, x4);         /* new e2 */

    la_vinsgr2vr_w(dst, x4, 1);   /* e2 */
    la_vinsgr2vr_w(dst, x6, 3);   /* a2 */

    ra_free_temp(vt2);
    ra_free_temp(vt0);
    ra_free_temp(x7);
    ra_free_temp(x6);
    ra_free_temp(x5);
    ra_free_temp(x4);
    ra_free_temp(x3);
    ra_free_temp(x2);
    ra_free_temp(x1);
    ra_free_temp_auto(src);
    return true;
}

bool translate_sha256msg1(IR1_INST *pir1)
{
    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);
    IR2_OPND dst = ra_alloc_xmm(ir1_opnd_base_reg_num(opnd0));
    IR2_OPND src;
    IR2_OPND t0 = ra_alloc_ftemp();
    IR2_OPND t1 = ra_alloc_ftemp();
    IR2_OPND t2 = ra_alloc_ftemp();
    IR2_OPND t3 = ra_alloc_ftemp();

    if (!ir1_opnd_is_mem(opnd1)) {
        src = ra_alloc_xmm(ir1_opnd_base_reg_num(opnd1));
    } else {
        src = ra_alloc_ftemp();
        assert(ir1_opnd_size(opnd1) == 128);
        load_freg128_from_ir1_mem(src, opnd1);
    }

    la_vbsrl_v(t0, dst, 4);
    la_vextrins_w(t0, src, VEXTRINS_IMM_4_0(3, 0));
    la_vrotri_w(t1, t0, 7);
    la_vrotri_w(t3, t0, 18);
    la_vxor_v(t1, t1, t3);
    la_vsrli_w(t3, t0, 3);
    la_vxor_v(t1, t1, t3);
    la_vadd_w(dst, dst, t1);

    ra_free_temp(t3);
    ra_free_temp(t2);
    ra_free_temp(t1);
    ra_free_temp(t0);
    ra_free_temp_auto(src);
    return true;
}

bool translate_sha256msg2(IR1_INST *pir1)
{
    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);
    IR2_OPND dst = ra_alloc_xmm(ir1_opnd_base_reg_num(opnd0));
    IR2_OPND src;
    IR2_OPND t0 = ra_alloc_ftemp();
    IR2_OPND t1 = ra_alloc_ftemp();
    IR2_OPND t2 = ra_alloc_ftemp();
    IR2_OPND t3 = ra_alloc_ftemp();

    if (!ir1_opnd_is_mem(opnd1)) {
        src = ra_alloc_xmm(ir1_opnd_base_reg_num(opnd1));
    } else {
        src = ra_alloc_ftemp();
        assert(ir1_opnd_size(opnd1) == 128);
        load_freg128_from_ir1_mem(src, opnd1);
    }

    la_vbsrl_v(t0, src, 8);
    la_vrotri_w(t1, t0, 17);
    la_vrotri_w(t3, t0, 19);
    la_vxor_v(t1, t1, t3);
    la_vsrli_w(t3, t0, 10);
    la_vxor_v(t1, t1, t3);
    la_vadd_w(dst, dst, t1);
    la_vbsll_v(t0, dst, 8);
    la_vrotri_w(t1, t0, 17);
    la_vrotri_w(t3, t0, 19);
    la_vxor_v(t1, t1, t3);
    la_vsrli_w(t3, t0, 10);
    la_vxor_v(t1, t1, t3);
    la_vadd_w(dst, dst, t1);

    ra_free_temp(t3);
    ra_free_temp(t2);
    ra_free_temp(t1);
    ra_free_temp(t0);
    ra_free_temp_auto(src);
    return true;
}
