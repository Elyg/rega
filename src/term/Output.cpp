#include "term/Output.h"

#include <iostream>

namespace rega::term
{

void writeRaw(std::string_view bytes, bool flush)
{
	std::cout.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
	if(flush)
	{
		std::cout.flush();
	}
}

} // namespace rega::term
