#ifndef CORE_SHADER_RECOMPILIER_TRANSLATION_INCLUDE_TRANSLATION_DISPATCHTHREADLIMIT_HPP
#define CORE_SHADER_RECOMPILIER_TRANSLATION_INCLUDE_TRANSLATION_DISPATCHTHREADLIMIT_HPP

#include "IntermediateRepresentation/IrProgram.hpp"

namespace ShaderRecompiler {

void LimitDispatchThreads(IrProgram& program);

}

#endif
