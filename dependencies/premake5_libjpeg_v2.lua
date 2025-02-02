dofile('rive_build_config.lua')

if _OPTIONS['no-rive-decoders'] then
    return
end

local dependency = require('dependency')
libjpeg = dependency.github('rive-app/libjpeg', 'v9f')

project('libjpeg')
do
    kind('StaticLib')

    includedirs({ libjpeg })

    files({
        libjpeg .. '/jaricom.c',
        libjpeg .. '/jcapimin.c',
        libjpeg .. '/jcapistd.c',
        libjpeg .. '/jcarith.c',
        libjpeg .. '/jccoefct.c',
        libjpeg .. '/jccolor.c',
        libjpeg .. '/jcdctmgr.c',
        libjpeg .. '/jchuff.c',
        libjpeg .. '/jcinit.c',
        libjpeg .. '/jcmainct.c',
        libjpeg .. '/jcmarker.c',
        libjpeg .. '/jcmaster.c',
        libjpeg .. '/jcomapi.c',
        libjpeg .. '/jcparam.c',
        libjpeg .. '/jcprepct.c',
        libjpeg .. '/jcsample.c',
        libjpeg .. '/jctrans.c',
        libjpeg .. '/jdapimin.c',
        libjpeg .. '/jdapistd.c',
        libjpeg .. '/jdarith.c',
        libjpeg .. '/jdatadst.c',
        libjpeg .. '/jdatasrc.c',
        libjpeg .. '/jdcoefct.c',
        libjpeg .. '/jdcolor.c',
        libjpeg .. '/jddctmgr.c',
        libjpeg .. '/jdhuff.c',
        libjpeg .. '/jdinput.c',
        libjpeg .. '/jdmainct.c',
        libjpeg .. '/jdmarker.c',
        libjpeg .. '/jdmaster.c',
        libjpeg .. '/jdmerge.c',
        libjpeg .. '/jdpostct.c',
        libjpeg .. '/jdsample.c',
        libjpeg .. '/jdtrans.c',
        libjpeg .. '/jerror.c',
        libjpeg .. '/jfdctflt.c',
        libjpeg .. '/jfdctfst.c',
        libjpeg .. '/jfdctint.c',
        libjpeg .. '/jidctflt.c',
        libjpeg .. '/jidctfst.c',
        libjpeg .. '/jidctint.c',
        libjpeg .. '/jquant1.c',
        libjpeg .. '/jquant2.c',
        libjpeg .. '/jutils.c',
        libjpeg .. '/jmemmgr.c',
        libjpeg .. '/jmemansi.c',
    })
end
