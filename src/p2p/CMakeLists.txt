# P2P Networking Library (Qt-based)
# Only built when ENABLE_P2P=ON and Qt6 is available

add_library(dinero_p2p
    ../daemon/p2p/PeerManagerQt.cpp
    peer.cpp
    peer_manager.cpp
    p2p_message.cpp
)

target_link_libraries(dinero_p2p
    PRIVATE
        Qt6::Core
        Qt6::Network
        Qt6::WebSockets
        dinero_common
        dinero_crypto
        jsoncpp_static
)

target_compile_definitions(dinero_p2p PRIVATE DINERO_ENABLE_P2P=1)

target_include_directories(dinero_p2p 
    PUBLIC 
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_CURRENT_LIST_DIR}
)

# Enable Qt MOC for Qt-based classes
set_target_properties(dinero_p2p PROPERTIES
    AUTOMOC ON
)
