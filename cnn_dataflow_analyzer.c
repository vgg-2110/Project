#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

/* ================================================================
 *  PHAN 1: DATAFLOW STRATEGY (OS / WS / IS)
 * ================================================================ */
static int DF_N, DF_M, DF_C, DF_H, DF_R, DF_S, DF_E;
static int DF_TM, DF_TC, DF_TE; 
static float *DF_IFMAP, *DF_FILTER, *DF_OFMAP;
static float *DF_LB_I, *DF_LB_F, *DF_LB_O;
static int g_tm, g_tc, g_te;

#define DF_I(n,c,h,w)   DF_IFMAP [((n)*DF_C +(c))*DF_H*DF_H +(h)*DF_H +(w)]
#define DF_F(m,c,i,j)   DF_FILTER[((m)*DF_C +(c))*DF_R*DF_R +(i)*DF_R +(j)]
#define DF_O(n,m,x,y)   DF_OFMAP [((n)*DF_M +(m))*DF_E*DF_E +(x)*DF_E +(y)]

#define DF_LBH          (g_te + DF_R - 1)
#define DF_LI(c,h,w)    DF_LB_I[(c)*DF_LBH*DF_LBH + (h)*DF_LBH + (w)]
#define DF_LF(m,c,i,j)  DF_LB_F[((m)*g_tc+(c))*DF_R*DF_R+(i)*DF_R+(j)]
#define DF_LO(m,x,y)    DF_LB_O[(m)*g_te*g_te + (x)*g_te + (y)]

static int df_cmin(int a, int b) { return a < b ? a : b; }

typedef struct {
    long load, compute, store, load_elems, store_elems;
    double time_sec;
} DFCounter;

static DFCounter df_cnt;
static void df_cnt_reset(void) { memset(&df_cnt, 0, sizeof(df_cnt)); }

static void df_load_ifmap(int n, int c0, int tc, int ex0, int ey0, int te) {
    int hx0 = ex0 * DF_S, hy0 = ey0 * DF_S, lbh = te + DF_R - 1, c_end = df_cmin(c0 + tc, DF_C);
    long elems = 0;
    for (int c = c0; c < c_end; c++) {
        for (int x = 0; x < lbh && (hx0+x) < DF_H; x++) {
            int yc = 0;
            for (int y = 0; y < lbh && (hy0+y) < DF_H; y++) {
                DF_LI(c-c0, x, y) = DF_I(n, c, hx0+x, hy0+y); yc++;
            }
            elems += yc;
        }
    }
    df_cnt.load_elems += elems; df_cnt.load++;
}

static void df_load_filter(int m0, int tm, int c0, int tc) {
    int m_end = df_cmin(m0+tm, DF_M), c_end = df_cmin(c0+tc, DF_C);
    for (int m = m0; m < m_end; m++)
        for (int c = c0; c < c_end; c++)
            for (int i = 0; i < DF_R; i++)
                for (int j = 0; j < DF_R; j++)
                    DF_LF(m-m0, c-c0, i, j) = DF_F(m, c, i, j);
    df_cnt.load_elems += (long)(m_end-m0)*(c_end-c0)*DF_R*DF_R; df_cnt.load++;
}

static void df_compute(int m0, int tm, int c0, int tc, int ex0, int ey0, int te) {
    int me = df_cmin(m0+tm, DF_M) - m0, exe = df_cmin(ex0+te, DF_E) - ex0;
    int eye = df_cmin(ey0+te, DF_E) - ey0, ce = df_cmin(c0+tc, DF_C) - c0;
    for (int m=0; m<me; m++) for (int ex=0; ex<exe; ex++) for (int ey=0; ey<eye; ey++)
    for (int c=0; c<ce; c++) for (int i=0; i<DF_R; i++) for (int j=0; j<DF_R; j++)
        DF_LO(m, ex, ey) += DF_LI(c, ex*DF_S+i, ey*DF_S+j) * DF_LF(m, c, i, j);
    df_cnt.compute++;
}

static void df_store(int n, int m0, int tm, int ex0, int ey0, int te) {
    int me = df_cmin(m0+tm, DF_M) - m0, exe = df_cmin(ex0+te, DF_E) - ex0, eye = df_cmin(ey0+te, DF_E) - ey0;
    for (int m=0; m<me; m++) for (int ex=0; ex<exe; ex++) for (int ey=0; ey<eye; ey++)
        DF_O(n, m0+m, ex0+ex, ey0+ey) += DF_LO(m, ex, ey);
    df_cnt.store_elems += (long)me * exe * eye; df_cnt.store++;
}

static void df_zero_ofmap(void) { memset(DF_OFMAP, 0, (size_t)DF_N*DF_M*DF_E*DF_E*sizeof(float)); }
static void df_zero_lb_o(int tm, int te) { memset(DF_LB_O, 0, (size_t)tm*te*te*sizeof(float)); }

static void run_os(void) {
    df_cnt_reset(); df_zero_ofmap(); clock_t t0 = clock();
    for (int n=0; n<DF_N; n++) for (int m0=0; m0<DF_M; m0+=DF_TM)
    for (int ex0=0; ex0<DF_E; ex0+=DF_TE) for (int ey0=0; ey0<DF_E; ey0+=DF_TE) {
        df_zero_lb_o(DF_TM, DF_TE);
        for (int c0=0; c0<DF_C; c0+=DF_TC) {
            df_load_ifmap(n, c0, DF_TC, ex0, ey0, DF_TE);
            df_load_filter(m0, DF_TM, c0, DF_TC);
            df_compute(m0, DF_TM, c0, DF_TC, ex0, ey0, DF_TE);
        }
        df_store(n, m0, DF_TM, ex0, ey0, DF_TE);
    }
    df_cnt.time_sec = (double)(clock()-t0)/CLOCKS_PER_SEC;
}

static void run_ws(void) {
    df_cnt_reset(); df_zero_ofmap(); clock_t t0 = clock();
    for (int n=0; n<DF_N; n++) for (int m0=0; m0<DF_M; m0+=DF_TM) for (int c0=0; c0<DF_C; c0+=DF_TC) {
        df_load_filter(m0, DF_TM, c0, DF_TC);
        for (int ex0=0; ex0<DF_E; ex0+=DF_TE) for (int ey0=0; ey0<DF_E; ey0+=DF_TE) {
            df_zero_lb_o(DF_TM, DF_TE);
            df_load_ifmap(n, c0, DF_TC, ex0, ey0, DF_TE);
            df_compute(m0, DF_TM, c0, DF_TC, ex0, ey0, DF_TE);
            df_store(n, m0, DF_TM, ex0, ey0, DF_TE);
        }
    }
    df_cnt.time_sec = (double)(clock()-t0)/CLOCKS_PER_SEC;
}

static void run_is(void) {
    df_cnt_reset(); df_zero_ofmap(); clock_t t0 = clock();
    for (int n=0; n<DF_N; n++) for (int ex0=0; ex0<DF_E; ex0+=DF_TE)
    for (int ey0=0; ey0<DF_E; ey0+=DF_TE) for (int c0=0; c0<DF_C; c0+=DF_TC) {
        df_load_ifmap(n, c0, DF_TC, ex0, ey0, DF_TE);
        for (int m0=0; m0<DF_M; m0+=DF_TM) {
            df_zero_lb_o(DF_TM, DF_TE);
            df_load_filter(m0, DF_TM, c0, DF_TC);
            df_compute(m0, DF_TM, c0, DF_TC, ex0, ey0, DF_TE);
            df_store(n, m0, DF_TM, ex0, ey0, DF_TE);
        }
    }
    df_cnt.time_sec = (double)(clock()-t0)/CLOCKS_PER_SEC;
}

/* ================================================================
 *  PHAN 2: FULL NETWORK (AlexNet-CIFAR10)
 * ================================================================ */
#define NET_IN_C 3
#define NET_IN_H 32
#define NET_IN_W 32
#define C1 32
#define C2 64
#define C3 128
#define C4 128
#define C5 128
#define FC1 512
#define FC2 256
#define FC3 10
#define FC1_IN (C5 * 4 * 4)

static int NET_TILE_H = 8, NET_TILE_W = 8;
typedef struct { long load, compute, store; } LayerCnt;
static LayerCnt lc_conv[5], lc_relu[5], lc_pool[3], lc_fc[3], lc_softmax;
static long net_total_load, net_total_compute, net_total_store;

typedef struct { int c, h, w; float *d; } Net_Tensor;
static Net_Tensor net_tcreate(int c, int h, int w) {
    Net_Tensor t = {c, h, w, (float*)calloc((size_t)c*h*w, sizeof(float))};
    return t;
}
static void net_tfree(Net_Tensor *t) { free(t->d); t->d=NULL; t->c=t->h=t->w=0; }

static float* net_load_f32(const char *path, int n) {
    FILE *f = fopen(path,"rb");
    if (!f) return NULL;
    float *x = (float*)malloc((size_t)n*sizeof(float));
    fread(x,sizeof(float),(size_t)n,f);
    fclose(f); return x;
}

static Net_Tensor net_conv2d(Net_Tensor x, float *w, float *b, int out_c, int K, int stride, int pad, LayerCnt *cnt) {
    int oh = (x.h - K + 2*pad) / stride + 1, ow = (x.w - K + 2*pad) / stride + 1;
    Net_Tensor y = net_tcreate(out_c, oh, ow);
    for (int oc=0; oc<out_c; oc++) for (int th=0; th<oh; th+=NET_TILE_H) for (int tw=0; tw<ow; tw+=NET_TILE_W) {
        int he = (th+NET_TILE_H < oh) ? th+NET_TILE_H : oh, we = (tw+NET_TILE_W < ow) ? tw+NET_TILE_W : ow;
        for (int ohi=th; ohi<he; ohi++) for (int owi=tw; owi<we; owi++) {
            cnt->load++; net_total_load++; float s = b[oc];
            for (int ic=0; ic<x.c; ic++) for (int kh=0; kh<K; kh++) for (int kw=0; kw<K; kw++) {
                int ih = ohi*stride + kh - pad, iw = owi*stride + kw - pad;
                if ((unsigned)ih < (unsigned)x.h && (unsigned)iw < (unsigned)x.w) {
                    s += x.d[(ic*x.h+ih)*x.w+iw] * w[((oc*x.c+ic)*K+kh)*K+kw];
                    cnt->load+=2; net_total_load+=2; cnt->compute+=2; net_total_compute+=2;
                }
            }
            cnt->store++; net_total_store++; y.d[(oc*y.h+ohi)*y.w+owi] = s;
        }
    }
    return y;
}

static void net_relu(Net_Tensor x, LayerCnt *cnt) {
    int n = x.c*x.h*x.w;
    for (int i=0; i<n; i++) {
        cnt->load++; net_total_load++; cnt->compute++; net_total_compute++; cnt->store++; net_total_store++;
        if (x.d[i] < 0) x.d[i] = 0;
    }
}

static Net_Tensor net_maxpool(Net_Tensor x, int K, int stride, int pad, LayerCnt *cnt) {
    int oh = (x.h-K+2*pad)/stride+1, ow = (x.w-K+2*pad)/stride+1;
    Net_Tensor y = net_tcreate(x.c, oh, ow);
    for (int c=0; c<x.c; c++) for (int ohi=0; ohi<oh; ohi++) for (int owi=0; owi<ow; owi++) {
        float m = -1e38f;
        for (int kh=0; kh<K; kh++) for (int kw=0; kw<K; kw++) {
            int ih = ohi*stride+kh-pad, iw = owi*stride+kw-pad;
            if ((unsigned)ih<(unsigned)x.h && (unsigned)iw<(unsigned)x.w) {
                float v = x.d[(c*x.h+ih)*x.w+iw];
                cnt->load++; net_total_load++; cnt->compute++; net_total_compute++;
                if (v>m) m=v;
            }
        }
        cnt->store++; net_total_store++; y.d[(c*y.h+ohi)*y.w+owi] = m;
    }
    return y;
}

static void net_fc(const float *x, const float *w, const float *b, float *y, int in_d, int out_d, int do_relu, LayerCnt *cnt) {
    for (int o=0; o<out_d; o++) {
        cnt->load++; net_total_load++; float s = b[o];
        for (int i=0; i<in_d; i++) {
            s += x[i]*w[o*in_d+i];
            cnt->load+=2; net_total_load+=2; cnt->compute+=2; net_total_compute+=2;
        }
        cnt->store++; net_total_store++; y[o] = do_relu && s<0 ? 0 : s;
    }
}

static void net_softmax(float *x, int n, LayerCnt *cnt) {
    float m = x[0];
    for (int i=1; i<n; i++) { cnt->load++; net_total_load++; cnt->compute++; net_total_compute++; if (x[i]>m) m=x[i]; }
    float s=0;
    for (int i=0; i<n; i++) {
        cnt->load++; net_total_load++; cnt->compute+=2; net_total_compute+=2; cnt->store++; net_total_store++;
        x[i]=expf(x[i]-m); s+=x[i];
    }
    for (int i=0; i<n; i++) { cnt->load++; net_total_load++; cnt->compute++; net_total_compute++; cnt->store++; net_total_store++; x[i]/=s; }
}

/* ================================================================
 *  PHAN 3: IN KET QUA VA GHI FILE
 * ================================================================ */
static void write_verify_files(float *snap_os, float *snap_ws, float *snap_is) {
    FILE *f = fopen("config.txt","w");
    fprintf(f,"%d %d %d %d %d %d %d\n", DF_N, DF_M, DF_C, DF_H, DF_R, DF_S, DF_E);
    fprintf(f,"0.0 0.0 0.0\n"); fclose(f);

    f = fopen("ifmap.txt","w"); for(int i=0; i<DF_N*DF_C*DF_H*DF_H; i++) fprintf(f,"%.6f\n", DF_IFMAP[i]); fclose(f);
    f = fopen("filter.txt","w"); for(int i=0; i<DF_M*DF_C*DF_R*DF_R; i++) fprintf(f,"%.6f\n", DF_FILTER[i]); fclose(f);
    
    size_t sz = (size_t)DF_N*DF_M*DF_E*DF_E;
    f = fopen("ofmap_c.txt","w"); for(size_t i=0; i<sz; i++) fprintf(f,"%.6f\n", snap_os[i]); fclose(f);
    f = fopen("ofmap_ws.txt","w"); for(size_t i=0; i<sz; i++) fprintf(f,"%.6f\n", snap_ws[i]); fclose(f);
    f = fopen("ofmap_is.txt","w"); for(size_t i=0; i<sz; i++) fprintf(f,"%.6f\n", snap_is[i]); fclose(f);
}

static void print_sep(int w) { for (int i=0; i<w; i++) printf("="); printf("\n"); }

static void print_dataflow_results(DFCounter *os, DFCounter *ws, DFCounter *is, float max_err_ws, float max_err_is) {
    printf("\n"); print_sep(72);
    printf(" DATAFLOW STRATEGY ANALYSIS (1 lop Conv2D don le)\n");
    printf(" N=%d C=%d M=%d H=%d R=%d S=%d E=%d  Tile: TM=%d TC=%d TE=%d\n", DF_N,DF_C,DF_M,DF_H,DF_R,DF_S,DF_E,DF_TM,DF_TC,DF_TE);
    print_sep(72);
    printf(" %-4s  %10s %10s %8s  %14s %14s  %10s\n", "Strat","Load","Compute","Store","LoadElems","StoreElems","Time(s)");
    printf(" %-4s  %10s %10s %8s  %14s %14s  %10s\n", "----","----","-------","-----","---------","----------","-------");
    #define PROW(name,c) printf(" %-4s  %10ld %10ld %8ld  %14ld %14ld  %10.6f\n", name,(c)->load,(c)->compute,(c)->store,(c)->load_elems,(c)->store_elems,(c)->time_sec)
    PROW("OS", os); PROW("WS", ws); PROW("IS", is);
    #undef PROW
    print_sep(72);
    printf("  OS vs WS: max_err=%.2e -> %s\n", max_err_ws, max_err_ws<0.01f?"PASS":"FAIL");
    printf("  OS vs IS: max_err=%.2e -> %s\n", max_err_is, max_err_is<0.01f?"PASS":"FAIL");
    print_sep(72);
}

static void print_network_results(int *tiles, int ntiles, double *avg_time, long *tot_load, long *tot_cmp, long *tot_sto) {
    printf("\n"); print_sep(82);
    printf(" FULL NETWORK ANALYSIS (AlexNet-CIFAR10)\n");
    print_sep(82);
    double base = avg_time[0];
    printf(" %-10s  %12s  %12s  %12s  %10s  %8s\n", "TileSize","Load","Compute","Store","Time(ms)","Speedup");
    for (int i=0; i<ntiles; i++) {
        char tname[16]; if (tiles[i]==1) snprintf(tname,16,"Baseline"); else snprintf(tname,16,"%dx%d",tiles[i],tiles[i]);
        printf(" %-10s  %12ld  %12ld  %12ld  %10.3f  %8.2fx\n", tname, tot_load[i], tot_cmp[i], tot_sto[i], avg_time[i], base/avg_time[i]);
    }
    print_sep(82);
}

static void mode_dataflow(void) {
    printf("\n[MODE: DATAFLOW STRATEGY ANALYSIS]\n");
    DF_N = 1; DF_C = 8; DF_M = 8; DF_H = 16; DF_R = 3; DF_S = 1; DF_E = 14;
    DF_TM=4; DF_TC=4; DF_TE=4;

    DF_IFMAP  = (float*)calloc((size_t)DF_N*DF_C*DF_H*DF_H, sizeof(float));
    DF_FILTER = (float*)calloc((size_t)DF_M*DF_C*DF_R*DF_R, sizeof(float));
    DF_OFMAP  = (float*)calloc((size_t)DF_N*DF_M*DF_E*DF_E, sizeof(float));
    int lbh = DF_TE + DF_R - 1;
    DF_LB_I = (float*)calloc((size_t)DF_TC*lbh*lbh, sizeof(float));
    DF_LB_F = (float*)calloc((size_t)DF_TM*DF_TC*DF_R*DF_R, sizeof(float));
    DF_LB_O = (float*)calloc((size_t)DF_TM*DF_TE*DF_TE, sizeof(float));

    for (int i=0; i<DF_N*DF_C*DF_H*DF_H; i++) DF_IFMAP[i] = (float)(rand()%100)/10.0f;
    for (int i=0; i<DF_M*DF_C*DF_R*DF_R; i++) DF_FILTER[i] = (float)(rand()%100)/10.0f - 5.0f;

    size_t osz = (size_t)DF_N*DF_M*DF_E*DF_E;
    float *snap_os = (float*)malloc(osz*sizeof(float)), *snap_ws = (float*)malloc(osz*sizeof(float)), *snap_is = (float*)malloc(osz*sizeof(float));

    g_tm=DF_TM; g_tc=DF_TC; g_te=DF_TE;
    run_os(); memcpy(snap_os, DF_OFMAP, osz*sizeof(float)); DFCounter os_res = df_cnt;
    run_ws(); memcpy(snap_ws, DF_OFMAP, osz*sizeof(float)); DFCounter ws_res = df_cnt;
    run_is(); memcpy(snap_is, DF_OFMAP, osz*sizeof(float)); DFCounter is_res = df_cnt;

    float max_ws=0, max_is=0;
    for (size_t i=0; i<osz; i++) {
        if (fabsf(snap_os[i]-snap_ws[i])>max_ws) max_ws=fabsf(snap_os[i]-snap_ws[i]);
        if (fabsf(snap_os[i]-snap_is[i])>max_is) max_is=fabsf(snap_os[i]-snap_is[i]);
    }
    print_dataflow_results(&os_res, &ws_res, &is_res, max_ws, max_is);
    write_verify_files(snap_os, snap_ws, snap_is);
    
    free(DF_IFMAP); free(DF_FILTER); free(DF_OFMAP); free(DF_LB_I); free(DF_LB_F); free(DF_LB_O);
    free(snap_os); free(snap_ws); free(snap_is);
}

static void mode_network(void) {
    printf("\n[MODE: FULL NETWORK - AlexNet CIFAR-10]\n");
    float *conv_w[5], *conv_b[5];
    int conv_oc[5] = {C1,C2,C3,C4,C5}, conv_ic[5] = {NET_IN_C,C1,C2,C3,C4};
    char wpath[128], bpath[128];

    for (int l=0; l<5; l++) {
        snprintf(wpath,128,"export_bin/conv%d.weight.bin",l+1); snprintf(bpath,128,"export_bin/conv%d.bias.bin",l+1);
        conv_w[l] = net_load_f32(wpath, conv_oc[l]*conv_ic[l]*3*3); conv_b[l] = net_load_f32(bpath, conv_oc[l]);
        if (!conv_w[l]||!conv_b[l]) { printf("Cannot load weights. Run export first.\n"); return; }
    }
    float *fc_w[3], *fc_b[3]; int fc_out[3] = {FC1,FC2,FC3}, fc_in[3] = {FC1_IN,FC1,FC2};
    for (int l=0; l<3; l++) {
        snprintf(wpath,128,"export_bin/fc%d.weight.bin",l+1); snprintf(bpath,128,"export_bin/fc%d.bias.bin",l+1);
        fc_w[l] = net_load_f32(wpath, fc_out[l]*fc_in[l]); fc_b[l] = net_load_f32(bpath, fc_out[l]);
    }

    Net_Tensor g_inp = net_tcreate(NET_IN_C, NET_IN_H, NET_IN_W);
    FILE *sf = fopen("export_bin/sample_input.raw","rb");
    if (!sf) return; fread(g_inp.d, sizeof(float), NET_IN_C*NET_IN_H*NET_IN_W, sf); fclose(sf);

    int tiles[5] = {1,4,8,16,32}, ntiles=5, N_RUNS=10, best_pred=-1;
    double avg_time[5]; long tot_load[5], tot_cmp[5], tot_sto[5]; float best_prob[10] = {0};

    for (int ti=0; ti<ntiles; ti++) {
        NET_TILE_H = tiles[ti]; NET_TILE_W = tiles[ti]; double total=0;
        for (int run=0; run<N_RUNS; run++) {
            net_total_load=net_total_compute=net_total_store=0; clock_t t0 = clock();
            Net_Tensor c1 = net_conv2d(g_inp, conv_w[0],conv_b[0],C1,3,1,1,&lc_conv[0]); net_relu(c1,&lc_relu[0]);
            Net_Tensor p1 = net_maxpool(c1,2,2,0,&lc_pool[0]); net_tfree(&c1);
            Net_Tensor c2 = net_conv2d(p1,conv_w[1],conv_b[1],C2,3,1,1,&lc_conv[1]); net_tfree(&p1); net_relu(c2,&lc_relu[1]);
            Net_Tensor p2 = net_maxpool(c2,2,2,0,&lc_pool[1]); net_tfree(&c2);
            Net_Tensor c3 = net_conv2d(p2,conv_w[2],conv_b[2],C3,3,1,1,&lc_conv[2]); net_tfree(&p2); net_relu(c3,&lc_relu[2]);
            Net_Tensor c4 = net_conv2d(c3,conv_w[3],conv_b[3],C4,3,1,1,&lc_conv[3]); net_tfree(&c3); net_relu(c4,&lc_relu[3]);
            Net_Tensor c5 = net_conv2d(c4,conv_w[4],conv_b[4],C5,3,1,1,&lc_conv[4]); net_tfree(&c4); net_relu(c5,&lc_relu[4]);
            Net_Tensor p5 = net_maxpool(c5,2,2,0,&lc_pool[2]); net_tfree(&c5);

            float out1[FC1], out2[FC2], out3[FC3];
            net_fc(p5.d,fc_w[0],fc_b[0],out1,FC1_IN,FC1,1,&lc_fc[0]); net_tfree(&p5);
            net_fc(out1,fc_w[1],fc_b[1],out2,FC1,FC2,1,&lc_fc[1]);
            net_fc(out2,fc_w[2],fc_b[2],out3,FC2,FC3,0,&lc_fc[2]);
            net_softmax(out3,FC3,&lc_softmax);

            total += 1000.0*(double)(clock()-t0)/CLOCKS_PER_SEC;
            if (run==0 && ti==0) {
                best_pred=0; for (int k=1;k<10;k++) if (out3[k]>out3[best_pred]) best_pred=k;
                memcpy(best_prob,out3,sizeof(out3));
            }
        }
        avg_time[ti] = total/N_RUNS; tot_load[ti] = net_total_load; tot_cmp[ti] = net_total_compute; tot_sto[ti] = net_total_store;
    }
    const char *cls[10] = {"airplane","automobile","bird","cat","deer","dog","frog","horse","ship","truck"};
    printf("\n Prediction: class=%d (%s)\n", best_pred, cls[best_pred]);
    print_network_results(tiles,ntiles,avg_time,tot_load,tot_cmp,tot_sto);
}

int main(int argc, char *argv[]) {
    const char *mode = (argc >= 2) ? argv[1] : "all";
    if (strcmp(mode,"dataflow")==0 || strcmp(mode,"all")==0) mode_dataflow();
    if (strcmp(mode,"network")==0 || strcmp(mode,"all")==0) mode_network();
    return 0;
}