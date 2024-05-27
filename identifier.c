#include "identifier.h"
#include <stdlib.h>

uint8_t* parse_identifier(int argc, char* argv[])
{
	size_t identifier_length;
	uint8_t* identifier;
	identifier_length = argc - 2;
	identifier = (uint8_t*)malloc(identifier_length * sizeof(uint8_t));
	if (identifier != NULL) {
		for (size_t i = 0; i < identifier_length; i++) {
			unsigned int temp;
			sscanf(argv[2U + i], "%x", &temp);
			identifier[i] = (uint8_t)temp;
		}
	}
	return identifier;
}
