idf_component_register(SRCS "websocket.c" "websocket_server.c"
	REQUIRES lwip mbedtls
	INCLUDE_DIRS "./include"
	EMBED_FILES server/test.css server/test.js server/root.html)
