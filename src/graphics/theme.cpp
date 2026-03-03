#include "theme.h"

namespace katux::graphics {

Theme& theme() {
    static Theme t;
    return t;
}

}