idf_component_register(SRCS "uc-rv32ima.c"
			"cache.c"
			"psram.c"
		       LDFRAGMENTS "link.lf"
                       INCLUDE_DIRS ".")
