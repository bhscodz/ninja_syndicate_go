import sys
from PIL import Image, ImageDraw
R="Ninja Adventure - Asset Pack/"
f,x0,y0,x1,y1,out=sys.argv[1],*map(int,sys.argv[2:6]),sys.argv[6]
S=int(sys.argv[7]) if len(sys.argv)>7 else 3
im=Image.open(R+f).convert("RGBA")
x1=min(x1,im.width//16);y1=min(y1,im.height//16)
w,h=(x1-x0)*16,(y1-y0)*16
bg=Image.new("RGBA",(w,h),(90,60,110,255))
bg.alpha_composite(im.crop((x0*16,y0*16,x1*16,y1*16)))
bg=bg.resize((w*S,h*S),Image.NEAREST)
pad=Image.new("RGBA",(w*S+24,h*S+14),(255,255,255,255));pad.paste(bg,(24,14))
d=ImageDraw.Draw(pad)
for c in range(x0,x1):
    X=24+(c-x0)*16*S; d.line([X,14,X,14+h*S],fill=(255,0,0,90)); d.text((X+2,1),str(c),fill=(0,0,0,255))
for r in range(y0,y1):
    Y=14+(r-y0)*16*S; d.line([24,Y,24+w*S,Y],fill=(255,0,0,90)); d.text((1,Y+2),str(r),fill=(0,0,0,255))
pad.convert("RGB").save(out)
