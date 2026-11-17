import sys; sys.path.insert(0,"tools")
from world_catalog import SPR, SHEETS, SHEET_FILE
from PIL import Image, ImageDraw
R="Ninja Adventure - Asset Pack/"
names=sys.argv[3:]
a,b=int(sys.argv[1]),int(sys.argv[2])
allm=[n for n in SPR]
names=allm[a:b]
cell=6*16*1+8
S=3
cols=6
sheets={n:Image.open(R+SHEET_FILE[n]).convert("RGBA") for n in SHEETS}
cw=(7*16)*S//2+10
rows=(len(names)+cols-1)//cols
out=Image.new("RGBA",(cols*cw*1,rows*(7*16*S//2+22)),(90,60,110,255))
d=ImageDraw.Draw(out)
for i,n in enumerate(names):
    sh,c,r,w,h=SPR[n]
    im=sheets[sh]
    # show 1-tile margin around the rect (clipped to sheet) with rect outline
    x0,y0=max(0,c-1),max(0,r-1); x1,y1=min(im.width//16,c+w+1),min(im.height//16,r+h+1)
    crop=im.crop((x0*16,y0*16,x1*16,y1*16)).resize(((x1-x0)*16*2,(y1-y0)*16*2),0)
    ox,oy=(i%cols)*cw,(i//cols)*(7*16*S//2+22)
    out.alpha_composite(crop,(ox+4,oy+16))
    d.rectangle([ox+4+(c-x0)*32,oy+16+(r-y0)*32,ox+4+(c-x0+w)*32-1,oy+16+(r-y0+h)*32-1],outline=(255,0,0,255))
    d.text((ox+4,oy+2),n,fill=(255,255,255,255))
out.convert("RGB").save(sys.argv[0].replace("chk.py","chk_%d.png"%a))
