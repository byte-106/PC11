/* jpg.h - minimal baseline JPEG decoder for PC11 (freestanding).
 *
 * Supports BASELINE sequential JPEG (SOF0), Huffman coded, 8-bit, the
 * common 1-component (grayscale) and 3-component YCbCr cases with
 * sampling 1x1 or 2x2 (i.e. 4:4:4 and 4:2:0). No progressive, no
 * arithmetic coding, no 12-bit. Good enough for typical photos/screens.
 *
 * decode_jpg() returns 1 on success and fills a caller-allocated RGBA
 * (0x00RRGGBB) buffer via the provided allocator.
 */
#ifndef PC11_JPG_H
#define PC11_JPG_H

typedef void* (*jpg_alloc_fn)(unsigned long);
typedef void  (*jpg_free_fn)(void*);

typedef struct {
    const unsigned char *d; unsigned long n, p;   /* input */
    unsigned int bits; int nbits;                  /* bit reader */
    int marker;                                    /* pending marker after fill */
} jbits;

/* Huffman table: fast lookup by building (code,len)->symbol */
typedef struct {
    unsigned char bits[17];      /* number of codes of each length 1..16 */
    unsigned char vals[256];     /* symbols in order */
    int   mincode[17], maxcode[18], valptr[17];
} jhuff;

typedef struct {
    int id, h, v, tq;            /* component id, sampling h/v, quant table */
    int td, ta;                  /* dc/ac huffman table selectors */
    int pred;                    /* DC predictor */
} jcomp;

typedef struct {
    jpg_alloc_fn alloc; jpg_free_fn freep;
    int W,H, ncomp;
    unsigned short qt[4][64];
    jhuff hdc[4], hac[4];
    jcomp comp[4];
    int restart_interval;
    int maxh, maxv;
} jstate;

static const unsigned char JZZ[64]={
 0,1,8,16,9,2,3,10,17,24,32,25,18,11,4,5,12,19,26,33,40,48,41,34,27,20,13,6,7,14,21,28,
 35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63};

static int j_u16(jstate*S,const unsigned char*d,unsigned long*p){(void)S; int v=(d[*p]<<8)|d[*p+1]; *p+=2; return v;}

static void jhuff_build(jhuff*h){
    int code=0,k=0;
    for(int l=1;l<=16;l++){
        h->valptr[l]=k; h->mincode[l]=code;
        code += h->bits[l]; k += h->bits[l];
        h->maxcode[l]= h->bits[l]? code-1 : -1;
        code <<= 1;
    }
    h->maxcode[17]=0x7fffffff;
}

static void jb_init(jbits*b,const unsigned char*d,unsigned long n,unsigned long p){
    b->d=d;b->n=n;b->p=p;b->bits=0;b->nbits=0;b->marker=0;
}
static int jb_bit(jbits*b){
    if(b->nbits==0){
        if(b->p>=b->n){ return 0; }
        unsigned char c=b->d[b->p++];
        if(c==0xFF){
            unsigned char c2= (b->p<b->n)? b->d[b->p]:0;
            if(c2==0){ b->p++; }                  /* stuffed 0xFF00 -> 0xFF */
            else { b->marker=c2; c=0; }           /* hit a marker */
        }
        b->bits=c; b->nbits=8;
    }
    int bit=(b->bits>>7)&1; b->bits<<=1; b->nbits--;
    return bit;
}
static int jb_bits(jbits*b,int n){ int v=0; for(int i=0;i<n;i++) v=(v<<1)|jb_bit(b); return v; }

static int jb_huff(jbits*b,jhuff*h){
    int code=0;
    for(int l=1;l<=16;l++){
        code=(code<<1)|jb_bit(b);
        if(h->maxcode[l]>=0 && code<=h->maxcode[l])
            return h->vals[h->valptr[l] + (code - h->mincode[l])];
    }
    return 0;
}
static int j_extend(int v,int t){ return (v < (1<<(t-1))) ? v - (1<<t) + 1 : v; }

/* clamp to 0..255 */
static int jclamp(int x){ return x<0?0:(x>255?255:x); }

/* Cosine basis: JCOS[x][u] = Cu * cos((2x+1)*u*pi/16) * 1024, integer.
 * Cu = 1/sqrt2 for u==0 else 1. (scaled by 1024 for fixed point) */
static const short JCOS[8][8] = {
 { 724, 1004,  946,  851,  724,  569,  392,  200},
 { 724,  851,  392, -200, -724,-1004, -946, -569},
 { 724,  569, -392,-1004, -724,  200,  946,  851},
 { 724,  200, -946, -569,  724,  851, -392,-1004},
 { 724, -200, -946,  569,  724, -851, -392, 1004},
 { 724, -569, -392, 1004, -724, -200,  946, -851},
 { 724, -851,  392,  200, -724, 1004, -946,  569},
 { 724,-1004,  946, -851,  724, -569,  392, -200},
};
/* separable integer IDCT on an 8x8 block (in-place). */
static void jidct(int*blk){
    int tmp[64];
    /* rows: for each row i, output[x] = sum_u in[i*8+u]*JCOS[x][u].
     * >>11 per pass gives the correct overall 1/4 IDCT normalization. */
    for(int i=0;i<8;i++){
        const int*b=blk+i*8;
        for(int x=0;x<8;x++){
            int sum=0;
            for(int u=0;u<8;u++) sum += b[u]*JCOS[x][u];
            tmp[i*8+x]=sum>>11;
        }
    }
    /* cols */
    for(int i=0;i<8;i++){
        for(int y=0;y<8;y++){
            int sum=0;
            for(int u=0;u<8;u++) sum += tmp[u*8+i]*JCOS[y][u];
            blk[y*8+i]=sum>>11;
        }
    }
}

static int decode_jpg(const unsigned char*data,unsigned long len,
                      jpg_alloc_fn alloc,jpg_free_fn freep,
                      unsigned int**out_px,int*out_w,int*out_h){
    jstate S; S.alloc=alloc; S.freep=freep; S.ncomp=0; S.restart_interval=0;
    for(int i=0;i<4;i++){ S.hdc[i].bits[0]=0; S.hac[i].bits[0]=0; }
    if(len<2||data[0]!=0xFF||data[1]!=0xD8) return 0;
    unsigned long p=2;
    unsigned char *out_y=NULL,*out_cb=NULL,*out_cr=NULL;
    unsigned int *px=NULL;

    while(p+4<=len){
        if(data[p]!=0xFF){ p++; continue; }
        unsigned char m=data[p+1]; p+=2;
        if(m==0xD9) break;                 /* EOI */
        if(m==0x01||(m>=0xD0&&m<=0xD7)) continue;
        int L=j_u16(&S,data,&p); unsigned long segend=p+L-2;
        if(segend>len) break;
        if(m==0xDB){                       /* DQT */
            while(p<segend){
                int pq=data[p]>>4, tq=data[p]&15; p++;
                for(int i=0;i<64;i++){ S.qt[tq][i]= pq? (unsigned short)j_u16(&S,data,&p) : data[p++]; }
            }
        } else if(m==0xC0||m==0xC1){       /* SOF0/1 baseline */
            p++; /* precision */
            S.H=j_u16(&S,data,&p); S.W=j_u16(&S,data,&p);
            S.ncomp=data[p++];
            S.maxh=1;S.maxv=1;
            for(int c=0;c<S.ncomp;c++){
                S.comp[c].id=data[p++];
                S.comp[c].h=data[p]>>4; S.comp[c].v=data[p]&15; p++;
                S.comp[c].tq=data[p++];
                if(S.comp[c].h>S.maxh)S.maxh=S.comp[c].h;
                if(S.comp[c].v>S.maxv)S.maxv=S.comp[c].v;
            }
        } else if(m==0xC2){
            return 0;                       /* progressive not supported */
        } else if(m==0xC4){                 /* DHT */
            while(p<segend){
                int tc=data[p]>>4, th=data[p]&15; p++;
                jhuff*h = tc? &S.hac[th] : &S.hdc[th];
                int total=0;
                for(int i=1;i<=16;i++){ h->bits[i]=data[p++]; total+=h->bits[i]; }
                for(int i=0;i<total;i++) h->vals[i]=data[p++];
                jhuff_build(h);
            }
        } else if(m==0xDD){                  /* DRI */
            S.restart_interval=j_u16(&S,data,&p);
        } else if(m==0xDA){                  /* SOS - scan */
            int ns=data[p++];
            for(int i=0;i<ns;i++){
                int cid=data[p++];
                int t=data[p++];
                for(int c=0;c<S.ncomp;c++) if(S.comp[c].id==cid){ S.comp[c].td=t>>4; S.comp[c].ta=t&15; }
            }
            p+=3; /* Ss, Se, Ah/Al */
            /* ---- decode entropy data ---- */
            if(S.W<=0||S.H<=0) return 0;
            int mcux=(S.W + 8*S.maxh -1)/(8*S.maxh);
            int mcuy=(S.H + 8*S.maxv -1)/(8*S.maxv);
            /* allocate per-component full-resolution planes */
            int compW[4],compH[4];
            for(int c=0;c<S.ncomp;c++){
                compW[c]=mcux*S.comp[c].h*8;
                compH[c]=mcuy*S.comp[c].v*8;
            }
            unsigned char*plane[4]={0,0,0,0};
            for(int c=0;c<S.ncomp;c++){ plane[c]=(unsigned char*)alloc((unsigned long)compW[c]*compH[c]); if(!plane[c]) goto fail; }

            jbits B; jb_init(&B,data,len,p);
            for(int c=0;c<S.ncomp;c++) S.comp[c].pred=0;
            int rcount=0;
            for(int my=0;my<mcuy;my++){
                for(int mx=0;mx<mcux;mx++){
                    for(int c=0;c<S.ncomp;c++){
                        for(int by=0;by<S.comp[c].v;by++){
                            for(int bx=0;bx<S.comp[c].h;bx++){
                                int blk[64]; for(int i=0;i<64;i++) blk[i]=0;
                                jhuff*hd=&S.hdc[S.comp[c].td];
                                jhuff*ha=&S.hac[S.comp[c].ta];
                                unsigned short*q=S.qt[S.comp[c].tq];
                                int t=jb_huff(&B,hd);
                                int diff= t? j_extend(jb_bits(&B,t),t):0;
                                S.comp[c].pred += diff;
                                blk[0]=S.comp[c].pred * q[0];
                                int k=1;
                                while(k<64){
                                    int rs=jb_huff(&B,ha);
                                    int r=rs>>4, s=rs&15;
                                    if(s==0){ if(r==15){k+=16;continue;} else break; }
                                    k+=r; if(k>=64)break;
                                    int val=j_extend(jb_bits(&B,s),s);
                                    blk[JZZ[k]]= val * q[k];
                                    k++;
                                }
                                jidct(blk);
                                /* place into plane */
                                int px0=(mx*S.comp[c].h+bx)*8;
                                int py0=(my*S.comp[c].v+by)*8;
                                for(int yy=0;yy<8;yy++)for(int xx=0;xx<8;xx++){
                                    int v=jclamp(blk[yy*8+xx]+128);
                                    int X=px0+xx, Y=py0+yy;
                                    if(X<compW[c]&&Y<compH[c]) plane[c][Y*compW[c]+X]=(unsigned char)v;
                                }
                            }
                        }
                    }
                    if(S.restart_interval && ++rcount==S.restart_interval){
                        rcount=0;
                        /* align to byte, skip RSTn marker */
                        B.nbits=0;
                        while(B.p+1<B.n && !(B.d[B.p]==0xFF && B.d[B.p+1]>=0xD0 && B.d[B.p+1]<=0xD7)) B.p++;
                        if(B.p+1<B.n) B.p+=2;
                        for(int c=0;c<S.ncomp;c++) S.comp[c].pred=0;
                        B.marker=0;
                    }
                }
            }
            /* ---- color convert to RGBA ---- */
            px=(unsigned int*)alloc((unsigned long)S.W*S.H*4);
            if(!px) goto fail;
            for(int y=0;y<S.H;y++){
                for(int x=0;x<S.W;x++){
                    int Yv,Cb=128,Cr=128;
                    /* sample each plane (account for subsampling) */
                    int yx = x*S.comp[0].h/S.maxh, yy=y*S.comp[0].v/S.maxv;
                    Yv=plane[0][yy*compW[0]+yx];
                    if(S.ncomp>=3){
                        int cx=x*S.comp[1].h/S.maxh, cy=y*S.comp[1].v/S.maxv;
                        Cb=plane[1][cy*compW[1]+cx];
                        int rx=x*S.comp[2].h/S.maxh, ry=y*S.comp[2].v/S.maxv;
                        Cr=plane[2][ry*compW[2]+rx];
                    }
                    int R,G,Bl;
                    if(S.ncomp>=3){
                        int cb=Cb-128, cr=Cr-128;
                        R=jclamp(Yv + ((91881*cr)>>16));
                        G=jclamp(Yv - ((22554*cb + 46802*cr)>>16));
                        Bl=jclamp(Yv + ((116130*cb)>>16));
                    } else { R=G=Bl=Yv; }
                    px[y*S.W+x]=((unsigned)R<<16)|((unsigned)G<<8)|(unsigned)Bl;
                }
            }
            for(int c=0;c<S.ncomp;c++) if(plane[c]) freep(plane[c]);
            *out_px=px; *out_w=S.W; *out_h=S.H;
            return 1;
        fail:
            for(int c=0;c<S.ncomp;c++) if(plane[c]) freep(plane[c]);
            if(px) freep(px);
            return 0;
        } else {
            /* skip unknown segment */
        }
        p=segend;
    }
    (void)out_y;(void)out_cb;(void)out_cr;
    return 0;
}

#endif
