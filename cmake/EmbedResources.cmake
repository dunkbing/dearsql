# EmbedResources.cmake
# Embeds fonts and images as C++ byte arrays

function(embed_fonts)
    # Embed font files as resources
    file(GLOB FONT_FILES "assets/fonts/*.ttf" "assets/fonts/*.otf")
    set(RESOURCE_FILE "${CMAKE_CURRENT_BINARY_DIR}/embedded_fonts.cpp")

    file(WRITE ${RESOURCE_FILE} "#include <cstdint>\n#include <cstddef>\n\n")
    file(APPEND ${RESOURCE_FILE} "struct EmbeddedFont {\n")
    file(APPEND ${RESOURCE_FILE} "    const char* name;\n")
    file(APPEND ${RESOURCE_FILE} "    const uint8_t* data;\n")
    file(APPEND ${RESOURCE_FILE} "    size_t size;\n")
    file(APPEND ${RESOURCE_FILE} "};\n\n")

    if(FONT_FILES)
        foreach(FONT_FILE ${FONT_FILES})
            get_filename_component(FONT_NAME ${FONT_FILE} NAME_WE)
            string(MAKE_C_IDENTIFIER ${FONT_NAME} FONT_VAR)

            file(READ ${FONT_FILE} FONT_DATA HEX)
            string(REGEX REPLACE "([0-9a-fA-F][0-9a-fA-F])" "0x\\1," FONT_DATA ${FONT_DATA})
            string(REGEX REPLACE ",$" "" FONT_DATA ${FONT_DATA})

            file(APPEND ${RESOURCE_FILE} "static const uint8_t ${FONT_VAR}_data[] = {${FONT_DATA}};\n")
        endforeach()

        file(APPEND ${RESOURCE_FILE} "\nstatic const EmbeddedFont embedded_fonts[] = {\n")
        foreach(FONT_FILE ${FONT_FILES})
            get_filename_component(FONT_NAME ${FONT_FILE} NAME_WE)
            string(MAKE_C_IDENTIFIER ${FONT_NAME} FONT_VAR)
            file(APPEND ${RESOURCE_FILE} "    {\"${FONT_NAME}\", ${FONT_VAR}_data, sizeof(${FONT_VAR}_data)},\n")
        endforeach()
        file(APPEND ${RESOURCE_FILE} "};\n\n")

        file(APPEND ${RESOURCE_FILE} "extern \"C\" const EmbeddedFont* getEmbeddedFonts() { return embedded_fonts; }\n")
        file(APPEND ${RESOURCE_FILE} "extern \"C\" size_t getEmbeddedFontCount() { return sizeof(embedded_fonts) / sizeof(embedded_fonts[0]); }\n")
    else()
        file(APPEND ${RESOURCE_FILE} "extern \"C\" const EmbeddedFont* getEmbeddedFonts() { return nullptr; }\n")
        file(APPEND ${RESOURCE_FILE} "extern \"C\" size_t getEmbeddedFontCount() { return 0; }\n")
    endif()

    # Make the resource file available to parent scope
    set(EMBEDDED_FONTS_FILE ${RESOURCE_FILE} PARENT_SCOPE)
endfunction()

function(embed_images)
    # Embed image files as resources
    file(GLOB IMAGE_FILES "assets/images/*.png" "assets/images/*.jpg" "assets/images/*.jpeg")
    set(IMAGE_RESOURCE_FILE "${CMAKE_CURRENT_BINARY_DIR}/embedded_images.cpp")

    file(WRITE ${IMAGE_RESOURCE_FILE} "#include <cstdint>\n#include <cstddef>\n\n")
    file(APPEND ${IMAGE_RESOURCE_FILE} "struct EmbeddedImage {\n")
    file(APPEND ${IMAGE_RESOURCE_FILE} "    const char* name;\n")
    file(APPEND ${IMAGE_RESOURCE_FILE} "    const uint8_t* data;\n")
    file(APPEND ${IMAGE_RESOURCE_FILE} "    size_t size;\n")
    file(APPEND ${IMAGE_RESOURCE_FILE} "};\n\n")

    if(IMAGE_FILES)
        foreach(IMAGE_FILE ${IMAGE_FILES})
            get_filename_component(IMAGE_NAME ${IMAGE_FILE} NAME_WE)
            string(MAKE_C_IDENTIFIER ${IMAGE_NAME} IMAGE_VAR)

            file(READ ${IMAGE_FILE} IMAGE_DATA HEX)
            string(REGEX REPLACE "([0-9a-fA-F][0-9a-fA-F])" "0x\\1," IMAGE_DATA ${IMAGE_DATA})
            string(REGEX REPLACE ",$" "" IMAGE_DATA ${IMAGE_DATA})

            file(APPEND ${IMAGE_RESOURCE_FILE} "static const uint8_t ${IMAGE_VAR}_data[] = {${IMAGE_DATA}};\n")
        endforeach()

        file(APPEND ${IMAGE_RESOURCE_FILE} "\nstatic const EmbeddedImage embedded_images[] = {\n")
        foreach(IMAGE_FILE ${IMAGE_FILES})
            get_filename_component(IMAGE_NAME ${IMAGE_FILE} NAME_WE)
            string(MAKE_C_IDENTIFIER ${IMAGE_NAME} IMAGE_VAR)
            file(APPEND ${IMAGE_RESOURCE_FILE} "    {\"${IMAGE_NAME}\", ${IMAGE_VAR}_data, sizeof(${IMAGE_VAR}_data)},\n")
        endforeach()
        file(APPEND ${IMAGE_RESOURCE_FILE} "};\n\n")

        file(APPEND ${IMAGE_RESOURCE_FILE} "extern \"C\" const EmbeddedImage* getEmbeddedImages() { return embedded_images; }\n")
        file(APPEND ${IMAGE_RESOURCE_FILE} "extern \"C\" size_t getEmbeddedImageCount() { return sizeof(embedded_images) / sizeof(embedded_images[0]); }\n")
    else()
        file(APPEND ${IMAGE_RESOURCE_FILE} "extern \"C\" const EmbeddedImage* getEmbeddedImages() { return nullptr; }\n")
        file(APPEND ${IMAGE_RESOURCE_FILE} "extern \"C\" size_t getEmbeddedImageCount() { return 0; }\n")
    endif()

    # Make the resource file available to parent scope
    set(EMBEDDED_IMAGES_FILE ${IMAGE_RESOURCE_FILE} PARENT_SCOPE)
endfunction()
