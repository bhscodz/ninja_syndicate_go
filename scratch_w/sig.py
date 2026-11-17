import sys
from PIL import Image
R="Ninja Adventure - Asset Pack/"
def sig(im, c, r, outc, thr=60):
    t=im.crop((c*16,r*16,c*16+16,r*16+16)).convert("RGBA")
    px=t.load()
    def out(x,y):
        n=0;tot=0
        for dx in(-1,0,1):
            for dy in(-1,0,1):
                p=px[min(15,max(0,x+dx)),min(15,max(0,y+dy))]
                if p[3]<50: continue
                tot+=1
                if sum(abs(p[i]-outc[i]) for i in range(3))<thr: n+=1
        return tot>0 and n*2>tot
    pts={'NW':(1,1),'N':(8,1),'NE':(14,1),'W':(1,8),'E':(14,8),'SW':(1,14),'S':(8,14),'SE':(14,14)}
    return {k:out(*v) for k,v in pts.items()}
if __name__=="__main__":
    f,x0,y0,x1,y1=sys.argv[1],*map(int,sys.argv[2:6])
    im=Image.open(R+f).convert("RGBA")
    oc=tuple(int(v) for v in sys.argv[6].split(','))
    thr=int(sys.argv[7]) if len(sys.argv)>7 else 60
    for r in range(y0,y1):
        for c in range(x0,x1):
            s=sig(im,c,r,oc,thr)
            g=lambda k:'o' if s[k] else '.'
            print(f"({c},{r}) {g('NW')}{g('N')}{g('NE')}/{g('W')}-{g('E')}/{g('SW')}{g('S')}{g('SE')}",end="  ")
        print()
