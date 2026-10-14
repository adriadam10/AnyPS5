#include "Translation/DispatchThreadLimit.hpp"
#include "IntermediateRepresentation/IrBuilder.hpp"
#include <algorithm>
#include <stdexcept>
#include <vector>

namespace ShaderRecompiler {

void LimitDispatchThreads(IrProgram& program) {
    if (program.BlockOrder().empty()) throw std::runtime_error("LimitDispatchThreads: the program has no blocks");
    IrBlock& entry = *program.BlockOrder().front();
    auto& instructions = entry.Instructions();
    const auto ballot = std::find_if(instructions.begin(), instructions.end(), [](const IrValue* value) { return value->Opcode() == IrOpcode::Ballot; });
    if (ballot == instructions.end()) throw std::runtime_error("LimitDispatchThreads: the entry block has no initial exec ballot");
    IrValue* initial = (*ballot)->Argument(0);
    IrBuilder ir(program);
    ir.SetInsertionPoint(entry);
    const auto builtin = [&ir](StageInputKind kind, std::uint32_t component) -> IrValue& {
        return ir.Emit(IrOpcode::GetBuiltin, IrOpcodeType(IrOpcode::GetBuiltin), {&ir.Constant(static_cast<std::uint32_t>(kind)), &ir.Constant(component)});
    };
    std::vector<IrValue*> emitted;
    IrValue* limited = initial;
    IrValue* first = nullptr;
    for (std::uint32_t axis = 0; axis < 3u; axis++) {
        IrValue& limit = builtin(StageInputKind::DispatchThreadLimit, axis);
        IrValue& id = builtin(StageInputKind::GlobalInvocationId, axis);
        IrValue& inside = ir.ULessThan(id, limit);
        limited = &ir.LogicalAnd(*limited, inside);
        if (first == nullptr) first = limited;
        emitted.insert(emitted.end(), {&limit, &id, &inside, limited});
    }
    for (IrValue* value : emitted) {
        entry.RemoveInstruction(value);
        entry.InsertInstructionBefore(*ballot, value);
    }
    const std::vector<IrUse> uses = initial->OperandUses();
    for (const IrUse& use : uses) {
        if (use.user != first) use.user->ReplaceArgument(use.operand, limited);
    }
}

}
