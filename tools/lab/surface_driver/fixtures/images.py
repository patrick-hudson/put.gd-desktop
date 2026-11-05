"""Known synthetic pixels, with no workstation or user payloads."""
import io
from tools.lab import lab
from ..catalog import image_modules
from .base import Fixture


def image(scope):
    scope.ctx.fixture('app-profile')
    scope.initialize()
    Image,_,_=image_modules()
    im=Image.new('RGB',(640,320))
    for i,color in enumerate(lab.FIXTURE_RGB):
        x,y=(i%2)*320,(i//2)*160
        im.paste(color,(x,y,x+320,y+160))
    output=io.BytesIO();im.save(output,format='PNG')
    path=scope.root+'/synthetic-'+scope.token[:8]+'.png'
    scope.write(path,output.getvalue())
    return Fixture(path=path,width=640,height=320,synthetic=True)
