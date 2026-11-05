"""Pixel checks shared by the graphical editor acceptance and surface recipes."""


def redactions_black(width, pixels, redactions):
    return all(pixels[(y*width+x)*3:(y*width+x)*3+3] == bytes((0,0,0))
               for rx,ry,rw,rh in redactions
               for y in range(ry,ry+rh) for x in range(rx,rx+rw))
