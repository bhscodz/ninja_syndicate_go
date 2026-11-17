import sys; sys.path.insert(0,"tools")
import autotile as a
from PIL import Image
import random, math
R=a.R
W,H=40,16
rnd=random.Random(3)
m=[[False]*W for _ in range(H)]
for cx,cy,r in [(5,7,4),(9,5,3),(13,9,3.5),(8,11,2.5)]:
    for y in range(H):
        for x in range(W):
            if (x-cx)**2+(y-cy)**2<=r*r: m[y][x]=True
m[7][6]=False  # hole
m[7][5]=False
m[3][15]=True; m[11][17]=True; m[11][18]=True
ms=a.smooth(m,W,H)
imgs={k:Image.open(R+("TilesetFloor.png" if v[0]=="Floor" else "TilesetWater.png")).convert("RGBA") for k,v in a.ORIGINS.items()}
out=Image.new("RGBA",(W*16*3,H*16*2),(255,0,255,255))
# render 3 sets per row ... simple: each set rendered at x offset 0..., show 6 sets in 2 rows x3 columns, cropping 20 cols
for i,(k,(sh,oc,orr)) in enumerate(a.ORIGINS.items()):
    ox=(i%3)*20*16; oy=(i//3)*H*16
    base=imgs["dirt"].crop((0,12*16,16,13*16))
    for y in range(H):
        for x in range(20):
            out.alpha_composite(base,(ox+x*16,oy+y*16))
            if ms[y][x]:
                c,r=a.pick(ms,x,y,W,H)
                out.alpha_composite(imgs[k].crop(((oc+c)*16,(orr+r)*16,(oc+c)*16+16,(orr+r)*16+16)),(ox+x*16,oy+y*16))
out=out.crop((0,0,60*16,32*16)).resize((60*16*2//2*1,32*16),Image.NEAREST)
out.convert("RGB").save("D:/cloud_computing_game/scratch_w/auto_test.png")
