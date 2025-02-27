//===- GenericCycleImpl.h -------------------------------------*- C++ -*---===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This template implementation resides in a separate file so that it
// does not get injected into every .cpp file that includes the
// generic header.
//
// DO NOT INCLUDE THIS FILE WHEN MERELY USING CYCLEINFO.
//
// This file should only be included by files that implement a
// specialization of the relevant templates. Currently these are:
// - CycleAnalysis.cpp
// - MachineCycleAnalysis.cpp
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_ADT_GENERICCYCLEIMPL_H
#define LLVM_ADT_GENERICCYCLEIMPL_H

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/ADT/GenericCycleInfo.h"

#define DEBUG_TYPE "generic-cycle-impl"

namespace llvm {

template <typename ContextT>
bool GenericCycle<ContextT>::contains(const GenericCycle *C) const {
  if (!C)
    return false;

  if (Depth > C->Depth)
    return false;
  while (Depth < C->Depth)
    C = C->ParentCycle;
  return this == C;
}

template <typename ContextT>
void GenericCycle<ContextT>::getExitBlocks(
    SmallVectorImpl<BlockT *> &TmpStorage) const {
  TmpStorage.clear();

  size_t NumExitBlocks = 0;
  for (BlockT *Block : blocks()) {
    llvm::append_range(TmpStorage, successors(Block));

    for (size_t Idx = NumExitBlocks, End = TmpStorage.size(); Idx < End;
         ++Idx) {
      BlockT *Succ = TmpStorage[Idx];
      if (!contains(Succ)) {
        auto ExitEndIt = TmpStorage.begin() + NumExitBlocks;
        if (std::find(TmpStorage.begin(), ExitEndIt, Succ) == ExitEndIt)
          TmpStorage[NumExitBlocks++] = Succ;
      }
    }

    TmpStorage.resize(NumExitBlocks);
  }
}

/// \brief Helper class for computing cycle information.
template <typename ContextT> class GenericCycleInfo<ContextT>::Compute {
  GenericCycleInfo &Info;

  struct DFSInfo {
    unsigned Start = 0; // DFS start; positive if block is found
    unsigned End = 0;   // DFS end

    DFSInfo() {}
    explicit DFSInfo(unsigned Start) : Start(Start) {}

    /// Whether this node is an ancestor (or equal to) the node \p Other
    /// in the DFS tree.
    bool isAncestorOf(const DFSInfo &Other) const {
      return Start <= Other.Start && Other.End <= End;
    }
  };

  DenseMap<BlockT *, DFSInfo> BlockDFSInfo;
  SmallVector<BlockT *, 8> BlockPreorder;

  friend struct GraphTraits<ContractedDomSubTree>;

  Compute(const Compute &) = delete;
  Compute &operator=(const Compute &) = delete;

public:
  Compute(GenericCycleInfo &Info) : Info(Info) {}

  void run(BlockT *EntryBlock);

  static void updateDepth(CycleT *SubTree);

private:
  void dfs(BlockT *EntryBlock);
};

template <typename ContextT>
auto GenericCycleInfo<ContextT>::getTopLevelParentCycle(
    const BlockT *Block) const -> CycleT * {
  auto MapIt = BlockMap.find(Block);
  if (MapIt == BlockMap.end())
    return nullptr;

  auto *C = MapIt->second;
  while (C->ParentCycle)
    C = C->ParentCycle;
  return C;
}

template <typename ContextT>
void GenericCycleInfo<ContextT>::moveToNewParent(CycleT *NewParent,
                                                 CycleT *Child) {
  auto &CurrentContainer =
      Child->ParentCycle ? Child->ParentCycle->Children : TopLevelCycles;
  auto Pos = llvm::find_if(CurrentContainer, [=](const auto &Ptr) -> bool {
    return Child == Ptr.get();
  });
  assert(Pos != CurrentContainer.end());
  NewParent->Children.push_back(std::move(*Pos));
  *Pos = std::move(CurrentContainer.back());
  CurrentContainer.pop_back();
  Child->ParentCycle = NewParent;
}

/// \brief Main function of the cycle info computations.
template <typename ContextT>
void GenericCycleInfo<ContextT>::Compute::run(BlockT *EntryBlock) {
  LLVM_DEBUG(errs() << "Entry block: " << Info.Context.print(EntryBlock)
                    << "\n");
  dfs(EntryBlock);

  SmallVector<BlockT *, 8> Worklist;

  for (BlockT *HeaderCandidate : llvm::reverse(BlockPreorder)) {
    const DFSInfo CandidateInfo = BlockDFSInfo.lookup(HeaderCandidate);

    for (BlockT *Pred : predecessors(HeaderCandidate)) {
      const DFSInfo PredDFSInfo = BlockDFSInfo.lookup(Pred);
      if (CandidateInfo.isAncestorOf(PredDFSInfo))
        Worklist.push_back(Pred);
    }
    if (Worklist.empty()) {
      continue;
    }

    // Found a cycle with the candidate as its header.
    LLVM_DEBUG(errs() << "Found cycle for header: "
                      << Info.Context.print(HeaderCandidate) << "\n");
    std::unique_ptr<CycleT> NewCycle = std::make_unique<CycleT>();
    NewCycle->appendEntry(HeaderCandidate);
    NewCycle->appendBlock(HeaderCandidate);
    Info.BlockMap.try_emplace(HeaderCandidate, NewCycle.get());

    // Helper function to process (non-back-edge) predecessors of a discovered
    // block and either add them to the worklist or recognize that the given
    // block is an additional cycle entry.
    auto ProcessPredecessors = [&](BlockT *Block) {
      LLVM_DEBUG(errs() << "  block " << Info.Context.print(Block) << ": ");

      bool IsEntry = false;
      for (BlockT *Pred : predecessors(Block)) {
        const DFSInfo PredDFSInfo = BlockDFSInfo.lookup(Pred);
        if (CandidateInfo.isAncestorOf(PredDFSInfo)) {
          Worklist.push_back(Pred);
        } else {
          IsEntry = true;
        }
      }
      if (IsEntry) {
        assert(!NewCycle->isEntry(Block));
        LLVM_DEBUG(errs() << "append as entry\n");
        NewCycle->appendEntry(Block);
      } else {
        LLVM_DEBUG(errs() << "append as child\n");
      }
    };

    do {
      BlockT *Block = Worklist.pop_back_val();
      if (Block == HeaderCandidate)
        continue;

      // If the block has already been discovered by some cycle
      // (possibly by ourself), then the outermost cycle containing it
      // should become our child.
      if (auto *BlockParent = Info.getTopLevelParentCycle(Block)) {
        LLVM_DEBUG(errs() << "  block " << Info.Context.print(Block) << ": ");

        if (BlockParent != NewCycle.get()) {
          LLVM_DEBUG(errs()
                     << "discovered child cycle "
                     << Info.Context.print(BlockParent->getHeader()) << "\n");
          // Make BlockParent the child of NewCycle.
          Info.moveToNewParent(NewCycle.get(), BlockParent);
          NewCycle->Blocks.insert(NewCycle->Blocks.end(),
                                  BlockParent->block_begin(),
                                  BlockParent->block_end());

          for (auto *ChildEntry : BlockParent->entries())
            ProcessPredecessors(ChildEntry);
        } else {
          LLVM_DEBUG(errs()
                     << "known child cycle "
                     << Info.Context.print(BlockParent->getHeader()) << "\n");
        }
      } else {
        Info.BlockMap.try_emplace(Block, NewCycle.get());
        assert(!is_contained(NewCycle->Blocks, Block));
        NewCycle->Blocks.push_back(Block);
        ProcessPredecessors(Block);
      }
    } while (!Worklist.empty());

    Info.TopLevelCycles.push_back(std::move(NewCycle));
  }

  // Fix top-level cycle links and compute cycle depths.
  for (auto *TLC : Info.toplevel_cycles()) {
    LLVM_DEBUG(errs() << "top-level cycle: "
                      << Info.Context.print(TLC->getHeader()) << "\n");

    TLC->ParentCycle = nullptr;
    updateDepth(TLC);
  }
}

/// \brief Recompute depth values of \p SubTree and all descendants.
template <typename ContextT>
void GenericCycleInfo<ContextT>::Compute::updateDepth(CycleT *SubTree) {
  for (CycleT *Cycle : depth_first(SubTree))
    Cycle->Depth = Cycle->ParentCycle ? Cycle->ParentCycle->Depth + 1 : 1;
}

/// \brief Compute a DFS of basic blocks starting at the function entry.
///
/// Fills BlockDFSInfo with start/end counters and BlockPreorder.
template <typename ContextT>
void GenericCycleInfo<ContextT>::Compute::dfs(BlockT *EntryBlock) {
  SmallVector<unsigned, 8> DFSTreeStack;
  SmallVector<BlockT *, 8> TraverseStack;
  unsigned Counter = 0;
  TraverseStack.emplace_back(EntryBlock);

  do {
    BlockT *Block = TraverseStack.back();
    LLVM_DEBUG(errs() << "DFS visiting block: " << Info.Context.print(Block)
                      << "\n");
    if (!BlockDFSInfo.count(Block)) {
      // We're visiting the block for the first time. Open its DFSInfo, add
      // successors to the traversal stack, and remember the traversal stack
      // depth at which the block was opened, so that we can correctly record
      // its end time.
      LLVM_DEBUG(errs() << "  first encountered at depth "
                        << TraverseStack.size() << "\n");

      DFSTreeStack.emplace_back(TraverseStack.size());
      llvm::append_range(TraverseStack, successors(Block));

      LLVM_ATTRIBUTE_UNUSED
      bool Added = BlockDFSInfo.try_emplace(Block, ++Counter).second;
      assert(Added);
      BlockPreorder.push_back(Block);
      LLVM_DEBUG(errs() << "  preorder number: " << Counter << "\n");
    } else {
      assert(!DFSTreeStack.empty());
      if (DFSTreeStack.back() == TraverseStack.size()) {
        LLVM_DEBUG(errs() << "  ended at " << Counter << "\n");
        BlockDFSInfo.find(Block)->second.End = Counter;
        DFSTreeStack.pop_back();
      } else {
        LLVM_DEBUG(errs() << "  already done\n");
      }
      TraverseStack.pop_back();
    }
  } while (!TraverseStack.empty());
  assert(DFSTreeStack.empty());

  LLVM_DEBUG(
    errs() << "Preorder:\n";
    for (int i = 0, e = BlockPreorder.size(); i != e; ++i) {
      errs() << "  " << Info.Context.print(BlockPreorder[i]) << ": " << i << "\n";
    }
  );
}

/// \brief Reset the object to its initial state.
template <typename ContextT> void GenericCycleInfo<ContextT>::clear() {
  TopLevelCycles.clear();
  BlockMap.clear();
}

/// \brief Compute the cycle info for a function.
template <typename ContextT>
void GenericCycleInfo<ContextT>::compute(FunctionT &F) {
  Compute Compute(*this);
  Context.setFunction(F);

  LLVM_DEBUG(errs() << "Computing cycles for function: " << F.getName()
                    << "\n");
  Compute.run(ContextT::getEntryBlock(F));

  assert(validateTree());
}

/// \brief Find the innermost cycle containing a given block.
///
/// \returns the innermost cycle containing \p Block or nullptr if
///          it is not contained in any cycle.
template <typename ContextT>
auto GenericCycleInfo<ContextT>::getCycle(const BlockT *Block) const
    -> CycleT * {
  auto MapIt = BlockMap.find(Block);
  if (MapIt != BlockMap.end())
    return MapIt->second;
  return nullptr;
}

/// \brief Validate the internal consistency of the cycle tree.
///
/// Note that this does \em not check that cycles are really cycles in the CFG,
/// or that the right set of cycles in the CFG were found.
template <typename ContextT>
bool GenericCycleInfo<ContextT>::validateTree() const {
  DenseSet<BlockT *> Blocks;
  DenseSet<BlockT *> Entries;

  auto reportError = [](const char *File, int Line, const char *Cond) {
    errs() << File << ':' << Line
           << ": GenericCycleInfo::validateTree: " << Cond << '\n';
  };
#define check(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      reportError(__FILE__, __LINE__, #cond);                                  \
      return false;                                                            \
    }                                                                          \
  } while (false)

  for (const auto *TLC : toplevel_cycles()) {
    for (const CycleT *Cycle : depth_first(TLC)) {
      if (Cycle->ParentCycle)
        check(is_contained(Cycle->ParentCycle->children(), Cycle));

      for (BlockT *Block : Cycle->Blocks) {
        auto MapIt = BlockMap.find(Block);
        check(MapIt != BlockMap.end());
        check(Cycle->contains(MapIt->second));
        check(Blocks.insert(Block).second); // duplicates in block list?
      }
      Blocks.clear();

      check(!Cycle->Entries.empty());
      for (BlockT *Entry : Cycle->Entries) {
        check(Entries.insert(Entry).second); // duplicate entry?
        check(is_contained(Cycle->Blocks, Entry));
      }
      Entries.clear();

      unsigned ChildDepth = 0;
      for (const CycleT *Child : Cycle->children()) {
        check(Child->Depth > Cycle->Depth);
        if (!ChildDepth) {
          ChildDepth = Child->Depth;
        } else {
          check(ChildDepth == Child->Depth);
        }
      }
    }
  }

  for (const auto &Entry : BlockMap) {
    BlockT *Block = Entry.first;
    for (const CycleT *Cycle = Entry.second; Cycle;
         Cycle = Cycle->ParentCycle) {
      check(is_contained(Cycle->Blocks, Block));
    }
  }

#undef check

  return true;
}

/// \brief Print the cycle info.
template <typename ContextT>
void GenericCycleInfo<ContextT>::print(raw_ostream &Out) const {
  for (const auto *TLC : toplevel_cycles()) {
    for (const CycleT *Cycle : depth_first(TLC)) {
      for (unsigned I = 0; I < Cycle->Depth; ++I)
        Out << "    ";

      Out << Cycle->print(Context) << '\n';
    }
  }
}

} // namespace llvm

#undef DEBUG_TYPE

#endif // LLVM_ADT_GENERICCYCLEIMPL_H
