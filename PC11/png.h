/* png.h - minimal PNG decoder for PC11 (freestanding, no libc).
 *
 * Supports the common case:
 *   - 8-bit color depth
 *   - color types 2 (RGB) and 6 (RGBA), and 0 (gray) / 4 (gray+alpha)
 *   - no interlacing (interlace method 0)
 *   - zlib/DEFLATE compressed IDAT (dynamic + fixed Huffman, stored blocks)
 *
 * It does NOT support: 16-bit depth, palette (type 3), Adam7 interlace.
 * decode_png() returns 1 on success and fills out a malloc'd RGBA buffer.
 *
 * Memory comes from a caller-provided allocator (UEFI AllocatePool wrapper).
 */
#ifndef PC11_PNG_H
#define PC11_PNG_H

/* caller provides these (wrappers around UEFI pool alloc + simple free) */
typedef void* (*png_alloc_fn)(unsigned long size);
typedef void  (*png_free_fn)(void* p);

/* ---------- tiny DEFLATE (inflate) ---------- */
typedef struct {
    const unsigned char *src; unsigned long src_len, src_pos;
    unsigned int bitbuf; int bitcnt;
    unsigned char *out; unsigned long out_cap, out_len;
} inflate_state;

static int infl_getbit(inflate_state *s){
    if(s->bitcnt==0){
        if(s->src_pos>=s->src_len) return -1;
        s->bitbuf = s->src[s->src_pos++];
        s->bitcnt = 8;
    }
    int b = s->bitbuf & 1;
    s->bitbuf >>= 1; s->bitcnt--;
    return b;
}
static int infl_getbits(inflate_state *s, int n){
    int v=0; for(int i=0;i<n;i++){ int b=infl_getbit(s); if(b<0) return -1; v |= (b<<i); } return v;
}
static void infl_out(inflate_state *s, unsigned char c){
    if(s->out_len < s->out_cap) s->out[s->out_len] = c;
    s->out_len++;
}

/* Huffman table built from code lengths (canonical). */
typedef struct { unsigned short counts[16]; unsigned short symbols[288]; } huff;
static void huff_build(huff *h, const unsigned char *lengths, int n){
    for(int i=0;i<16;i++) h->counts[i]=0;
    for(int i=0;i<n;i++) h->counts[lengths[i]]++;
    h->counts[0]=0;
    unsigned short offs[16]; offs[0]=0; offs[1]=0;
    for(int i=1;i<15;i++) offs[i+1]=offs[i]+h->counts[i];
    for(int i=0;i<n;i++) if(lengths[i]) h->symbols[offs[lengths[i]]++]=i;
}
static int huff_decode(inflate_state *s, huff *h){
    int code=0, first=0, index=0;
    for(int len=1; len<=15; len++){
        int b=infl_getbit(s); if(b<0) return -1;
        code |= b;
        int count=h->counts[len];
        if(code - first < count) return h->symbols[index + (code-first)];
        index += count; first += count; first <<= 1; code <<= 1;
    }
    return -1;
}

static const unsigned short LEN_BASE[29]={3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
static const unsigned char  LEN_EXTRA[29]={0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
static const unsigned short DIST_BASE[30]={1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
static const unsigned char  DIST_EXTRA[30]={0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

static int inflate_block(inflate_state *s, huff *lh, huff *dh){
    for(;;){
        int sym = huff_decode(s, lh);
        if(sym<0) return -1;
        if(sym==256) return 0;            /* end of block */
        if(sym<256){ infl_out(s,(unsigned char)sym); continue; }
        sym -= 257; if(sym>=29) return -1;
        int len = LEN_BASE[sym] + infl_getbits(s, LEN_EXTRA[sym]);
        int dsym = huff_decode(s, dh);
        if(dsym<0||dsym>=30) return -1;
        int dist = DIST_BASE[dsym] + infl_getbits(s, DIST_EXTRA[dsym]);
        for(int i=0;i<len;i++){
            if((unsigned long)dist>s->out_len) return -1;
            unsigned char c = (s->out_len-dist < s->out_cap) ? s->out[s->out_len-dist] : 0;
            infl_out(s,c);
        }
    }
}

static int inflate_raw(const unsigned char *src, unsigned long slen,
                       unsigned char *out, unsigned long out_cap, unsigned long *out_len){
    inflate_state s; s.src=src; s.src_len=slen; s.src_pos=0;
    s.bitbuf=0; s.bitcnt=0; s.out=out; s.out_cap=out_cap; s.out_len=0;
    int final=0;
    while(!final){
        final = infl_getbit(&s); if(final<0) break;
        int type = infl_getbits(&s, 2); if(type<0) return 0;
        if(type==0){
            /* stored */
            s.bitcnt=0;
            if(s.src_pos+4>s.src_len) return 0;
            int len = s.src[s.src_pos] | (s.src[s.src_pos+1]<<8);
            s.src_pos+=4;
            for(int i=0;i<len;i++){ if(s.src_pos>=s.src_len) return 0; infl_out(&s, s.src[s.src_pos++]); }
        } else if(type==1){
            /* fixed Huffman */
            huff lh, dh;
            unsigned char ll[288];
            for(int i=0;i<144;i++)ll[i]=8;
            for(int i=144;i<256;i++)ll[i]=9;
            for(int i=256;i<280;i++)ll[i]=7;
            for(int i=280;i<288;i++)ll[i]=8;
            unsigned char dl[30]; for(int i=0;i<30;i++)dl[i]=5;
            huff_build(&lh,ll,288); huff_build(&dh,dl,30);
            if(inflate_block(&s,&lh,&dh)<0) return 0;
        } else if(type==2){
            /* dynamic Huffman */
            int hlit = infl_getbits(&s,5)+257;
            int hdist= infl_getbits(&s,5)+1;
            int hclen= infl_getbits(&s,4)+4;
            static const int ord[19]={16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
            unsigned char cll[19]; for(int i=0;i<19;i++) cll[i]=0;
            for(int i=0;i<hclen;i++) cll[ord[i]]=(unsigned char)infl_getbits(&s,3);
            huff ch; huff_build(&ch,cll,19);
            unsigned char lens[288+32]; int n=0;
            while(n<hlit+hdist){
                int sym=huff_decode(&s,&ch); if(sym<0) return 0;
                if(sym<16){ lens[n++]=(unsigned char)sym; }
                else if(sym==16){ int r=infl_getbits(&s,2)+3; unsigned char p=n?lens[n-1]:0; while(r-->0&&n<hlit+hdist) lens[n++]=p; }
                else if(sym==17){ int r=infl_getbits(&s,3)+3;  while(r-->0&&n<hlit+hdist) lens[n++]=0; }
                else if(sym==18){ int r=infl_getbits(&s,7)+11; while(r-->0&&n<hlit+hdist) lens[n++]=0; }
                else return 0;
            }
            huff lh,dh; huff_build(&lh,lens,hlit); huff_build(&dh,lens+hlit,hdist);
            if(inflate_block(&s,&lh,&dh)<0) return 0;
        } else return 0;
    }
    *out_len = s.out_len;
    return 1;
}

/* ---------- PNG container ---------- */
static unsigned int be32(const unsigned char *p){
    return ((unsigned)p[0]<<24)|((unsigned)p[1]<<16)|((unsigned)p[2]<<8)|p[3];
}
static int paeth(int a,int b,int c){
    int p=a+b-c, pa=p>a?p-a:a-p, pb=p>b?p-b:b-p, pc=p>c?p-c:c-p;
    if(pa<=pb && pa<=pc) return a;
    if(pb<=pc) return b;
    return c;
}

/* decode a PNG into a freshly-allocated RGBA (0xAARRGGBB stored as 32-bit
 * 0x00RRGGBB ignoring alpha) buffer. returns 1 on success. */
static int decode_png(const unsigned char *data, unsigned long len,
                      png_alloc_fn alloc, png_free_fn freep,
                      unsigned int **out_px, int *out_w, int *out_h){
    if(len<8) return 0;
    static const unsigned char sig[8]={137,80,78,71,13,10,26,10};
    for(int i=0;i<8;i++) if(data[i]!=sig[i]) return 0;
    unsigned long pos=8;
    int W=0,H=0,bitdepth=0,colortype=0,interlace=0;

    /* gather IDAT into one buffer */
    unsigned long idat_cap = len;     /* upper bound */
    unsigned char *idat = (unsigned char*)alloc(idat_cap);
    if(!idat) return 0;
    unsigned long idat_len=0;

    while(pos+8<=len){
        unsigned int clen=be32(data+pos); pos+=4;
        const unsigned char *ctype=data+pos; pos+=4;
        if(pos+clen+4>len) break;
        if(ctype[0]=='I'&&ctype[1]=='H'&&ctype[2]=='D'&&ctype[3]=='R'){
            W=(int)be32(data+pos); H=(int)be32(data+pos+4);
            bitdepth=data[pos+8]; colortype=data[pos+9]; interlace=data[pos+12];
        } else if(ctype[0]=='I'&&ctype[1]=='D'&&ctype[2]=='A'&&ctype[3]=='T'){
            for(unsigned int i=0;i<clen;i++) idat[idat_len++]=data[pos+i];
        } else if(ctype[0]=='I'&&ctype[1]=='E'&&ctype[2]=='N'&&ctype[3]=='D'){
            break;
        }
        pos += clen + 4; /* skip data + CRC */
    }
    if(W<=0||H<=0||bitdepth!=8||interlace!=0){ freep(idat); return 0; }
    int channels;
    if(colortype==2) channels=3;
    else if(colortype==6) channels=4;
    else if(colortype==0) channels=1;
    else if(colortype==4) channels=2;
    else { freep(idat); return 0; }

    /* zlib: skip 2-byte header, inflate the rest */
    if(idat_len<3){ freep(idat); return 0; }
    unsigned long raw_cap = (unsigned long)H*(1+(unsigned long)W*channels) + 16;
    unsigned char *raw = (unsigned char*)alloc(raw_cap);
    if(!raw){ freep(idat); return 0; }
    unsigned long raw_len=0;
    int ok = inflate_raw(idat+2, idat_len-2, raw, raw_cap, &raw_len);
    freep(idat);
    if(!ok){ freep(raw); return 0; }

    /* unfilter scanlines */
    unsigned int *px = (unsigned int*)alloc((unsigned long)W*H*4);
    if(!px){ freep(raw); return 0; }
    int stride = W*channels;
    unsigned char *prev = (unsigned char*)alloc(stride);
    if(!prev){ freep(raw); freep(px); return 0; }
    for(int i=0;i<stride;i++) prev[i]=0;
    unsigned long rp=0;
    for(int y=0;y<H;y++){
        if(rp>=raw_len){ freep(raw); freep(prev); return 1 ; }
        int filt = raw[rp++];
        unsigned char *line = &raw[rp]; rp += stride;
        for(int x=0;x<stride;x++){
            int a = (x>=channels)? line[x-channels] : 0;
            int b = prev[x];
            int c = (x>=channels)? prev[x-channels] : 0;
            int v = line[x];
            switch(filt){
                case 1: v += a; break;
                case 2: v += b; break;
                case 3: v += (a+b)/2; break;
                case 4: v += paeth(a,b,c); break;
                default: break;
            }
            line[x]=(unsigned char)v;
        }
        for(int x=0;x<W;x++){
            int r,g,bl;
            const unsigned char *p=&line[x*channels];
            if(channels>=3){ r=p[0]; g=p[1]; bl=p[2]; }
            else { r=g=bl=p[0]; }
            px[y*W+x] = ((unsigned)r<<16)|((unsigned)g<<8)|(unsigned)bl;
        }
        for(int i=0;i<stride;i++) prev[i]=line[i];
    }
    freep(raw); freep(prev);
    *out_px=px; *out_w=W; *out_h=H;
    return 1;
}

#endif
