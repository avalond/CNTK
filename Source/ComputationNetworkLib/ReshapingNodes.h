//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.md file in the project root for full license information.
//
// ReshapingNodes.h -- collection of nodes that reshape or sub-sample matrices leading to layout changes
//
#pragma once

#include "Basics.h"
#include "Matrix.h"
#include "ComputationNode.h"
#include "Sequences.h"

#include <unordered_set>
#include <map>
#include <string>
#include <vector>
#include <stdexcept>
#include <list>
#include <memory>
#include <algorithm>
#include <assert.h>
#include <atomic>
#include <sstream>
#include <iostream>

namespace Microsoft { namespace MSR { namespace CNTK {

// -----------------------------------------------------------------------
// ReinterpretNodeBase (input) -- base class for nodes that reinterpret
// -----------------------------------------------------------------------

template <class ElemType>
class ReinterpretNodeBase : public ComputationNode<ElemType>, public NumInputs<1>
{
    typedef ComputationNode<ElemType> Base;
    UsingComputationNodeMembers;

public:
    //DeclareConstructorFromConfigWithNumInputs(ReinterpretNodeBase);
    ReinterpretNodeBase(DEVICEID_TYPE deviceId, const wstring& name)
        : Base(deviceId, name)
    {
    }

    // stack K consecutive frames into a single frame that is K times taller
    // FrameRange and MBLayout refer to the 'to' (reduced) timeline.
    // BUGBUG: THIS IS UNTESTED!!
    static void Stack(const FrameRange& fr, const shared_ptr<MBLayout>& pMBLayout, /*const*/ Matrix<ElemType>& from, Matrix<ElemType>& to, size_t K, bool addTo)
    {
        // example
        //  input: T=2, D=2, K=3, S=2 (abcdef and uvwxyz)
        //   abc def
        //   ABC DEF
        //
        //   uvw xyz
        //   UVW XYZ
        //  target:
        //   a d
        //   A D
        //   b e
        //   B E
        //   c f
        //   C F
        //
        //   u x
        //   U X
        //   v y
        //   V Y
        //   w z
        //   W Z
        // underlying matrix storage is actually this:
        //  input:
        //   aubvcw dxeyfz
        //   AUBVCW DXEYFZ
        //  target:
        //   abcuvw defxyz
        //   ABCUVW DEFXYZ

        // I.e. this operation swaps index dimensions of a tensor:
        //   The input is a tensor of the form (D,       S, M, K, T).
        //   The output is of the form         (D, K, M, S,       T).
        //     K = stacking factor
        //     T = target steps
        //     S = #sequences
        //     D = featDim
        //     M = 1, thrown in for generality of underlying Matrix function

        // We operate on the 'to' layout, fr refers to result, not the input.
        // The input layout is different, but reshaping the input to output dimensions will allow us to pull out the right values anyway.
        auto from0 = from.Reshaped(to.GetNumRows(), to.GetNumCols()); // we operate on 'to' layout
        auto fromSlice0 = DataWithMBLayoutFor(from0, fr, pMBLayout);
        auto toSlice0 = DataWithMBLayoutFor(to, fr, pMBLayout);
        // now we got views on the right ranges of values, but with weird dimensions

        // reshape them into a unified view with D being the row dimension, and (S,M,K,T) the column dimension
        size_t D = from.GetNumRows();
        size_t SMKT = from.GetNumCols();
        auto fromSlice = fromSlice0.Reshaped(D, SMKT);
        auto toSlice = toSlice0.Reshaped(D, SMKT);

        // now to the shuffle dance
        size_t S = pMBLayout->GetNumParallelSequences();
        size_t T = pMBLayout->GetNumTimeSteps();
        size_t M = 1;
        Matrix<ElemType>::TensorShuffleScaleAndAdd(addTo ? 1.0f : 0, fromSlice, D, S, M, K, T, 1.0f, toSlice, toSlice);
    }

    // split frames of D*K elements into K consecutive frames of dimension D.
    // FrameRange and MBLayout refer to the 'from' (reduced) timeline.
    // This function is the inverse of Stack(). See comments there and exchange from and to.
    static void Unstack(const FrameRange& fr, const shared_ptr<MBLayout>& pMBLayout, /*const*/ Matrix<ElemType>& from, Matrix<ElemType>& to, size_t K, bool addTo)
    {
        auto fromSlice0 = DataWithMBLayoutFor(from, fr, pMBLayout);
        auto to0 = to.Reshaped(from.GetNumRows(), from.GetNumCols());
        auto toSlice0 = DataWithMBLayoutFor(to0, fr, pMBLayout);

        size_t D = to.GetNumRows();
        size_t SMKT = to.GetNumCols();
        auto fromSlice = fromSlice0.Reshaped(D, SMKT);
        auto toSlice = toSlice0.Reshaped(D, SMKT);

        size_t S = pMBLayout->GetNumParallelSequences();
        size_t T = pMBLayout->GetNumTimeSteps();
        size_t M = 1;
        Matrix<ElemType>::TensorShuffleScaleAndAdd(addTo ? 1.0f : 0, fromSlice, D, K, M, S, T, 1.0f, toSlice, toSlice);
    }
};

#define UsingReinterpretNodeBaseMembers UsingComputationNodeMembersBoilerplate

// TODO: This ReshapeNode is currently not used. Its function will be taken over by Transpose and the Reshape that follows this one below.

// -----------------------------------------------------------------------
// DeprecatedReshapeNode (input) -- reinterpret input matrix as having different dimensions
// where the new row dimension is given, and the column dimension is inferred.
// Also optionally associate a different TensorShape with the data.
//
// If input has no layout, then this reshapes the input matrix
// from (rows x cols) to (newRows x (cols / newRows * rows)).
//
// If input has a layout, then it adds or removes a nested time dimension.
//  - If newRows > rows, then we remove a time dimension by stacking all frames from the dimension into one:
//       (rows x (newRows/rows nested time steps) x T time steps)
//    -> (newRows x T time steps).
//  - If newRows < rows, then we add a time dimension, going
//       (rows x T time steps)
//    -> (newRows x (rows/newRows nested time steps) x T time steps).
//    which requires the nested time sequence to have the correct number of steps.
// E.g. going from rows=20 to newRows=40 assumes a nested time sequence of 2 steps, which are grouped into one step, with the two vectors stacked.
// Multiple parallel sequences are treated independently.
// TODO: This definition is poor; we should use a different node name, and specify the factor directly.
//       We may hide that in BrainScript, but better use different node types.
//       E.g. ReinterpretRowStackAsSequence and ReinterpretSequenceAsRowStack.
// BUGBUG: This is not actually implemented yet. Instead, it goes from 1 to K steps or from K to 1 step. This is temporary/experimental, until the plumbing for nesting is there.
//
// Thirdly, DeprecatedReshapeNode can also be used to update only the TensorShape. In that case, the MBLayout is kept as is.
//
// Note: The new row dimension must be a straight multiple or divisor of the current row dimension.
// To reshape to a non-multiple go to row dim 1 first.
//
// Unlike most other nodes, this node has intimate inside knowlegde of MBLayouts and frameRanges.
// TODO: Changing the TensorShape does not seem to belong here.
// -----------------------------------------------------------------------

template <class ElemType>
class DeprecatedReshapeNode : public ReinterpretNodeBase<ElemType>
{
    typedef ReinterpretNodeBase<ElemType> Base;
    UsingReinterpretNodeBaseMembers;
    static const std::wstring TypeName()
    {
        return L"DeprecatedReshape";
    }

public:
    DeprecatedReshapeNode(DEVICEID_TYPE deviceId, const wstring& name, size_t numRows = 0, const TensorShape& imageLayout = TensorShape())
        : Base(deviceId, name),
          m_numTargetRows(numRows),
          m_targetImageLayout(imageLayout)
    {
    }
    DeprecatedReshapeNode(const ScriptableObjects::IConfigRecordPtr configp)
        : DeprecatedReshapeNode(configp->Get(L"deviceId"), L"<placeholder>", configp->Get(L"numRows"), ImageDimensions::AsTensorShape(configp->Get(L"imageWidth"), configp->Get(L"imageHeight"), configp->Get(L"imageChannels"), ImageLayoutKind::HWC /*legacy*/))
    {
        // BUGBUG: We should not operate on image layouts here, but on a proper tensor layout.
        AttachInputs(configp, this->GetExpectedNumInputs());
    }

    virtual void CopyTo(ComputationNodeBasePtr nodeP, const std::wstring& newName, const CopyNodeFlags flags) const override
    {
        Base::CopyTo(nodeP, newName, flags);
        if (flags & CopyNodeFlags::copyNodeValue)
        {
            auto node = dynamic_pointer_cast<DeprecatedReshapeNode<ElemType>>(nodeP);
            node->m_numTargetRows = m_numTargetRows;
            node->m_targetImageLayout = m_targetImageLayout;
        }
    }

    virtual void Save(File& fstream) const override
    {
        Base::Save(fstream);
        fstream << m_numTargetRows;
        m_targetImageLayout.Save(fstream);
    }

    virtual void Load(File& fstream, size_t modelVersion) override
    {
        Base::Load(fstream, modelVersion);
        fstream >> m_numTargetRows;
        m_targetImageLayout.Load(fstream, /*acceptLegacyFormat=*/true);
    }

    virtual void /*IComputationNode::*/ PrintSelfBeforeValidation() const override
    {
        fprintf(stderr, "\nValidating --> %ls = %ls", NodeName().c_str(), OperationName().c_str());
        fprintf(stderr, "(");
        for (size_t i = 0; i < GetNumInputs(); i++)
        {
            ComputationNodePtr child = Input(i);
            if (i > 0)
                fprintf(stderr, ", ");
            if (!child)
                fprintf(stderr, "NULL");
            else
                fprintf(stderr, "%ls[%s%s]", child->NodeName().c_str(), string(child->GetSampleLayout()).c_str(), child->HasMBLayout() ? " x *" : "");
        }
        fprintf(stderr, ", NumOfRows=%lu, imageWidth=%lu, imageHeight=%lu, imageChannels=%lu)", m_numTargetRows, m_targetImageLayout[1], m_targetImageLayout[2], m_targetImageLayout[0]);
        // BUGBUG: This interpretaion as image dims is only correct for the 'legacy format, not for cudnn.
    }

    virtual void /*ComputationNodeBase::*/ Validate(bool isFinalValidationPass) override
    {
        Base::Validate(isFinalValidationPass);
        if (factor() == 1) // canonical case: keeps the MBLayout(e.g. only changing the TensorShape)
            m_pMBLayout = Input(0)->GetMBLayout();
        else if (Input(0)->HasMBLayout())
        {
            if (!m_pMBLayout)
                m_pMBLayout = make_shared<MBLayout>(); // mini-batch data: this generates a new layout
        }
        else
            assert(!m_pMBLayout); // reshaping non-mini-batch data

        size_t newCols = 1; // dummy
        if (!m_pMBLayout)
        {
            size_t rows = Input(0)->GetAsMatrixNumRows(), cols = Input(0)->GetAsMatrixNumCols();
            newCols = cols * rows / m_numTargetRows;
            if (isFinalValidationPass)
            {
                if ((m_numTargetRows > rows && m_numTargetRows % rows != 0) || // grouping columns
                    (m_numTargetRows < rows && rows % m_numTargetRows != 0))   // splitting columns
                    InvalidArgument("%ls %ls operation: output row dimension %d is not an integer multiple or divisor of input dimension %d", NodeName().c_str(), OperationName().c_str(), (int) m_numTargetRows, (int) rows);
                if (rows * cols != m_numTargetRows * newCols)
                    LogicError("%ls %ls operation: unexpected dimension mismatch", NodeName().c_str(), OperationName().c_str());
            }
        }

        // patch up m_targetImageLayout, which was originally a construction parameter
        InferTargetSampleLayout();

        // setting any dimension to 0 means lose the tensor, flatten to vector
        if (m_targetImageLayout.GetNumElements() == 0)
        {
            if (Input(0)->HasSampleLayout())
                fprintf(stderr, "WARNING: Reshape operation cannot inherit image size information from its child. Image size info is lost.\n");
            // TODO: We need to decide what reshaping means in presence of a tensor.
            if (HasMBLayout())
                SetDims(TensorShape(m_numTargetRows), true);
            else
                SetDims(TensorShape(m_numTargetRows, newCols), false);
        }
        else
        {
            if (m_numTargetRows != m_targetImageLayout.GetNumElements())
                LogicError("DeprecatedReshapeNode: InferTargetSampleLayout() computed a sample layout [%s] that mismatches m_numTargetRows %d.", string(m_targetImageLayout).c_str(), (int) m_numTargetRows);
            SetDims(m_targetImageLayout, HasMBLayout());
        }
    }

#if 0
    virtual void UpdateFunctionMBSize() override
    {
        size_t rows = Input(0)->GetNumRows(), cols = Input(0)->GetNumCols();
        size_t newCols = cols * rows / m_numTargetRows;
        if (!m_pMBLayout)
        {
#if 0
                VerifyDims(m_numTargetRows, newCols);
#endif
        }
        else
            SetNumCols(newCols);
    }
#endif

    // TODO: Clarify/resolve the semantic overlap between BeginForwardProp() and UpdateFunctionMBSize().
    virtual void /*IComputationNode::*/ BeginForwardProp() override
    {
        // create the derived layout
        if (m_pMBLayout && factor() != 1)
        {
            // BUGBUG: This assumes that the layout is complete at this point in time (RecurrentNodeBase makes the same assumption).
            //         This assumption is correct at present, but will becomes invalid once we go sequence-to-sequence.
            if (weStack())
            {
                // going from many samples to one: layout entry will get no flags
                if (Input(0)->GetMBLayout()->GetNumTimeSteps() * Input(0)->GetSampleMatrixNumRows() / m_numTargetRows != 1)
                    LogicError("DeprecatedReshapeNode::BeginForwardProp() faking to remove a nested time dimension only works when going back to a single frame per sequence.");
                // we are in frame mode now
                m_pMBLayout->InitAsFrameMode(Input(0)->GetNumParallelSequences());
            }
            else
            {
                // going from one sample to many: layout will get SentenceStart/SentenceEnd flags for the sequence we expand into
                if (Input(0)->GetMBLayout()->GetNumTimeSteps() != 1)
                    LogicError("DeprecatedReshapeNode::BeginForwardProp() faking to add a nested time dimension only works when coming from a single frame per sequence.");
                m_pMBLayout->Init(Input(0)->GetNumParallelSequences(), Input(0)->GetMBLayout()->GetNumTimeSteps() * Input(0)->GetSampleMatrixNumRows() / m_numTargetRows);
                for (size_t s = 0; s < m_pMBLayout->GetNumParallelSequences(); s++)
                    m_pMBLayout->AddSequence(NEW_SEQUENCE_ID, s, 0, GetMBLayout()->GetNumTimeSteps());
                // BUGBUG: In the future, NEW_SEQUENCE_ID will be incorrect here; need an iterator over sequences in there.
            }
        }
        // Call this at the end because this will resize Value(), but that requires the updated MBLayout. TODO: Clarify the sequence of events. Should we update the MBLayout in UpdateFunctionMBSize()?
        Base::BeginForwardProp();
    }

    // notes:
    //  - input and output have different time base and different layouts (unless the canonical case of factor() == 1)
    //  - fr refers to *functionValues*, not the inputs
    virtual void /*ComputationNode::*/ ForwardProp(const FrameRange& fr) override
    {
        size_t rows = Input(0)->Value().GetNumRows(), cols = Input(0)->Value().GetNumCols();
        size_t newCols = cols * rows / m_numTargetRows;
        assert(newCols * m_numTargetRows == cols * rows); // follows from above check
        Value().VerifySize(m_numTargetRows, newCols);

        // no layout case: this is indeed just a reshape. Same for canonical case
        // (We still need to copy the values since there is currently no way to point to an input function value while reshaping at the same time.)
        if (!m_pMBLayout || factor() == 1)
        {
            Value().Reshaped(newCols * m_numTargetRows, 1).SetValue(Input(0)->Value().Reshaped(cols * rows, 1)); // copy the values as one long vector
        }
        // layout case: reshape semantics happens across parallel seqeunces, i.e. requiring data shuffling
        else
        {
            // TODO: It does not make sense to run DeprecatedReshapeNode frame-by-frame inside a loop, because it changes the time base.
            //       However, in the future, we should be able to run inside an outer loop.
            if (!fr.IsAllFrames())
                InvalidArgument("%ls %ls operation cannot be run from inside a loop since it changes the time base.", NodeName().c_str(), OperationName().c_str());
            if (weStack())
                Base::Stack(fr, m_pMBLayout, Input(0)->Value(), Value(), factor(), false /*addTo*/);
            else
                Base::Unstack(fr.WithLayout(Input(0)->GetMBLayout()), Input(0)->GetMBLayout(), Input(0)->Value(), Value(), factor(), false /*addTo*/);
        }
    }

    virtual void /*ComputationNode::*/ BackpropTo(const size_t /*inputIndex*/, const FrameRange& fr) override
    {
        size_t rows = Input(0)->Value().GetNumRows(), cols = Input(0)->Value().GetNumCols();
        size_t newCols = cols * rows / m_numTargetRows;

        // no layout case: this is indeed just a reshape. Same for canonical case
        if (!m_pMBLayout || factor() == 1)
        {
            Input(0)->Gradient().Reshaped(cols * rows, 1) += Gradient().Reshaped(newCols * m_numTargetRows, 1); // treat the values as one long vector
        }
        // layout case: reshape semantics happens across parallel seqeunces, i.e. requiring data shuffling
        else
        {
            if (weStack())
                Base::Unstack(fr, m_pMBLayout, Gradient(), Input(0)->Gradient(), factor(), true /*addTo*/);
            else
                Base::Stack(fr.WithLayout(Input(0)->GetMBLayout()), Input(0)->GetMBLayout(), Gradient(), Input(0)->Gradient(), factor(), true /*addTo*/);
        }
    }

    virtual bool OutputUsedInComputingInputNodesGradients() const override
    {
        // The DeprecatedReshapeNode does not require its output value for computing
        // the gradients of its input nodes
        return false;
    }

    virtual bool InputUsedInComputingInputNodesGradients(size_t childIndex) const override
    {
        // The DeprecatedReshapeNode does not require any of it's input's values for computing
        // the gradients of its input nodes
        UNREFERENCED_PARAMETER(childIndex);
        return false;
    }

private:
    size_t m_numTargetRows;
    bool weStack() const
    {
        return m_numTargetRows > Input(0)->GetSampleMatrixNumRows();
    } // do we stack (multiple frames into one)
    size_t factor() const
    {
        return m_numTargetRows > Input(0)->GetSampleMatrixNumRows() ? m_numTargetRows / Input(0)->GetSampleMatrixNumRows() : Input(0)->GetSampleMatrixNumRows() / m_numTargetRows;
    } // factor by which we stack or unstack
    TensorShape m_targetImageLayout;

    // This infers dimensions in m_targetImageLayout.
    // Users are allowed to provide 2 (out of 3) image dimensions.
    // One missing dimension can be inferred. If two dimensions are
    // unspecified it throws a runtime error.
    void InferTargetSampleLayout()
    {
        // BUGBUG: Below is the result of refactoring and only works for rank-3 tensors. Generalize.
        if (m_targetImageLayout[1] > 0)
        {
            if (m_targetImageLayout[2] > 0)
            {
                if (m_targetImageLayout[0] > 0)
                {
                    if (m_targetImageLayout.GetNumElements() != m_numTargetRows)
                        RuntimeError("Image dimensions do not match row size.");
                }
                else
                {
                    if (m_numTargetRows % (m_targetImageLayout[1] * m_targetImageLayout[2]) > 0)
                        RuntimeError("Image row size is not a multiple of specified image dimensions.");
                    else
                        m_targetImageLayout = TensorShape(m_numTargetRows / (m_targetImageLayout[1] * m_targetImageLayout[2]), m_targetImageLayout[1], m_targetImageLayout[2]);
                }
            }
            else
            {
                if (m_targetImageLayout[0] > 0)
                {
                    if (m_numTargetRows % (m_targetImageLayout[1] * m_targetImageLayout[0]) > 0)
                        RuntimeError("Image row size is not a multiple of specified image dimensions.");
                    else
                        m_targetImageLayout = TensorShape(m_targetImageLayout[0], m_targetImageLayout[1], m_numTargetRows / (m_targetImageLayout[1] * m_targetImageLayout[0]));
                }
                else
                {
                    RuntimeError("At least two image dimensions must be specified.");
                }
            }
        }
        else
        {
            if (m_targetImageLayout[2] > 0)
            {
                if (m_targetImageLayout[0] > 0)
                {
                    if (m_numTargetRows % (m_targetImageLayout[2] * m_targetImageLayout[0]) > 0)
                        RuntimeError("Image row size is not a multiple of specified image dimensions.");
                    else
                        m_targetImageLayout = TensorShape(m_targetImageLayout[0], m_numTargetRows / (m_targetImageLayout[2] * m_targetImageLayout[0]), m_targetImageLayout[2]);
                }
                else
                    RuntimeError("At least two image dimensions must be specified.");
            }
            else if (m_targetImageLayout[0] > 0)
                RuntimeError("At least two image dimensions must be specified.");
            else
                m_targetImageLayout = TensorShape(1, m_numTargetRows, 1);
        }
    }
};

template class DeprecatedReshapeNode<float>;
template class DeprecatedReshapeNode<double>;

// -----------------------------------------------------------------------
// Reshape(x, tensorShape, beginDim=0, endDim=0) -- reinterpret input samples as having different tensor dimensions
//  - just replaces metadata m_sampleLayout, does not change data values
//  - one dimension may be specified as 0 and will be inferred
//  - optional beginDim/endDim denote to only replace a sub-range of dims, for implementing ReshapeDimension() and FlattenRank()
//  - may not be applied to time; use Permute() or Transpose()
//
// Derived operations:
//
// ReshapeDimension(x, dim, tensorShape) = Reshape(x, tensorShape, beginDim=dim, endDim=dim+1)
//  - reinterprets one dimension as multiple, where the number of elements remains the same
//  - one of the new dimensions may be specified as 0 and will be inferred
//
// FlattenDimensions(x, dim, num) = Reshape(x, 0, beginDim=dim, endDim=dim+num)
//  - replace two or more consecutive dims by a single dim with the same number of elements
//
// SplitDimension(x, dim, N) = ReshapeDimension(x, dim, 0:N)
//  - splits a dimension into a new tensor dimension, injecting them into a new dimension
//  - to split stacked frames into a new time dimension:
//    insert new time dim with ReshapeDimension(., -1, 0:1), SplitDimension(., dim, N), Transpose(., dim+1, -1), then Select(., dim+1, 0) away the new time dim
//    This would make 4 copies presently. We may need a compound C++ node for now.
//  - note: to split into multiple outputs (like tf.split()), use a BrainScript loop with Slice().
// -----------------------------------------------------------------------

template <class ElemType>
class ReshapeNode : public UnaryElementWiseNode<ElemType>
{
    typedef UnaryElementWiseNode<ElemType> Base;
    UsingUnaryElementwiseNodeBaseMembers;
    static const std::wstring TypeName()
    {
        return L"Reshape";
    }

public:
    ReshapeNode(DEVICEID_TYPE deviceId, const wstring& name, const TensorShape& replacementSampleLayout = TensorShape(), int beginDim = 1, int endDim = 0)
        : Base(deviceId, name),
          m_replacementSampleLayout(replacementSampleLayout),
          m_beginDimParameter(beginDim),
          m_endDimParameter(endDim)
    {
    }
    ReshapeNode(const ScriptableObjects::IConfigRecordPtr configp)
        : ReshapeNode(configp->Get(L"deviceId"), L"<placeholder>", configp->Get(L"shape"), configp->Get(L"beginDim"), configp->Get(L"endDim"))
    {
        AttachInputs(configp, this->GetExpectedNumInputs());
    }

    virtual void CopyTo(ComputationNodeBasePtr nodeP, const std::wstring& newName, const CopyNodeFlags flags) const override
    {
        Base::CopyTo(nodeP, newName, flags);
        if (flags & CopyNodeFlags::copyNodeValue)
        {
            auto node = dynamic_pointer_cast<ReshapeNode<ElemType>>(nodeP);
            node->m_replacementSampleLayout = m_replacementSampleLayout;
        }
    }

    virtual void Save(File& fstream) const override
    {
        Base::Save(fstream);
        fstream << m_beginDimParameter << m_endDimParameter;
        m_replacementSampleLayout.Save(fstream);
    }

    virtual void Load(File& fstream, size_t modelVersion) override
    {
        Base::Load(fstream, modelVersion);
        fstream >> m_beginDimParameter >> m_endDimParameter;
        m_replacementSampleLayout.Load(fstream);
    }

    virtual void /*ComputationNodeBase::*/ Validate(bool isFinalValidationPass) override
    {
        Base::Validate(isFinalValidationPass);

        // BUGBUG: For inputs without MBLayout, the sample layout should include the column dimension, but it does not currently. Needs to be fleshed out.
        const auto& inputSampleLayout = Input(0)->GetSampleLayout();
        const auto& inputDims = inputSampleLayout.GetDims();

        auto replacementDims = m_replacementSampleLayout.GetDims();

        size_t beginDim = m_beginDimParameter > 0 ? m_beginDimParameter - 1 : 0;
        size_t endDim = m_endDimParameter > 0 ? m_endDimParameter - 1 : inputDims.size();
        if (!isFinalValidationPass) // non-final: be tolerant, no errors
        {
            if (endDim > inputDims.size())
                endDim = inputDims.size();
            if (beginDim > endDim)
                beginDim = endDim;
        }

        // TODO: We should allow to reduce to a 0-length tensor if the dimension is 0

        // if a dimension is specified as zero then infer it, otherwise verify that total #elements matches
        size_t inputElements = 1; // get #elements in range to be replaced
        for (size_t k = beginDim; k < endDim; k++)
            inputElements *= inputDims[k];
        size_t targetElements = 1; // check/infer #elements to replace with
        size_t zeroIndex = SIZE_MAX;
        for (size_t k = 0; k < replacementDims.size(); k++)
        {
            if (replacementDims[k] != 0)
                targetElements *= replacementDims[k];
            else if (zeroIndex == SIZE_MAX)
                zeroIndex = k;
            else
                InvalidArgument("%ls %ls operation: More than one dimension was specified as zero in the replacement (sub-)dimensions [%s]", NodeName().c_str(), OperationName().c_str(), string(m_replacementSampleLayout).c_str());
        }
        if (zeroIndex != SIZE_MAX)
            replacementDims[zeroIndex] = inputElements / targetElements; // infer the number (ignore errors at this point)

        // assemble actual full dimension vector
        SmallVector<size_t> dims;
        dims.append(inputDims.begin(), inputDims.begin() + beginDim);
        dims.append(replacementDims.begin(), replacementDims.end());
        dims.append(inputDims.begin() + endDim, inputDims.end());
        auto sampleLayout = TensorShape(dims);

        // validate total dimension
        if (isFinalValidationPass && inputSampleLayout.GetNumElements() != sampleLayout.GetNumElements())
        {
            auto subShape = TensorShape(std::vector<size_t>(inputDims.begin() + beginDim, inputDims.begin() + endDim));
            InvalidArgument("%ls %ls operation: Input (sub-)dimensions [%s] incompatible with desired (sub-)dimensions [%s]. Number of elements %s.",
                            NodeName().c_str(), OperationName().c_str(),
                            string(subShape).c_str(), string(m_replacementSampleLayout).c_str(),
                            zeroIndex == SIZE_MAX ? "must be the same" : "is not an integer multiple of the non-0 dimensions");
        }

        // that's it
        SetDims(sampleLayout, HasMBLayout());
    }

    virtual void /*ComputationNode::*/ ForwardProp(const FrameRange& fr) override
    {
        ValueFor(fr).SetValue(Input(0)->ValueFor(fr));
    }

    virtual void /*ComputationNode::*/ BackpropTo(const size_t inputIndex, const FrameRange& fr) override
    {
        Input(inputIndex)->GradientFor(fr).SetValue(GradientFor(fr));
    }

    virtual bool OutputUsedInComputingInputNodesGradients() const override
    {
        return false;
    }
    virtual bool InputUsedInComputingInputNodesGradients(size_t /*childIndex*/) const override
    {
        return false;
    }

private:
    TensorShape m_replacementSampleLayout; // user-specified dimensions to replace dimensions [beginDim, endDim]
    int m_beginDimParameter;               // 1-based index range as specified
    int m_endDimParameter;
};

template class ReshapeNode<float>;
template class ReshapeNode<double>;

// -----------------------------------------------------------------------
// ReconcileMBLayout (dataInput, layoutInput)
// This node copies data from 'dataInput' while it propagates the minibatch-layout information from 'layoutInput'.
// It does perform a runtime check to enforce that the layout of 'dataInput' is compatible (identical content) to that of 'layoutInput'.
// This node is meant to be used from BrainScript macros that bracket expand/reduce pairs of nodes. It is not meant to really be used directly.
// TODO: What to do with sequence-boundary flags?
// -----------------------------------------------------------------------

template <class ElemType>
class ReconcileMBLayoutNode : public ComputationNode<ElemType>, public NumInputs<2>
{
    typedef ComputationNode<ElemType> Base;
    UsingComputationNodeMembersBoilerplate;
    static const std::wstring TypeName()
    {
        return L"ReconcileMBLayout";
    }

public:
    DeclareConstructorFromConfigWithNumInputs(ReconcileMBLayoutNode);
    ReconcileMBLayoutNode(DEVICEID_TYPE deviceId, const wstring& name)
        : Base(deviceId, name)
    {
    }

    virtual void /*ComputationNode::*/ BackpropTo(const size_t /*inputIndex*/, const FrameRange& fr) override
    {
        Input(0)->GradientFor(fr.WithLayout(Input(0)->GetMBLayout())) += GradientFor(fr);
        // TODO: Once we do in-place, the above must include a copy-to-self check (pay special attention to adding vs. copying).
    }

    virtual bool OutputUsedInComputingInputNodesGradients() const override
    {
        return false;
    }
    virtual bool InputUsedInComputingInputNodesGradients(size_t /*childIndex*/) const override
    {
        return false;
    }

    virtual void /*ComputationNode::*/ ForwardProp(const FrameRange& fr) override
    {
        // enforce compatibility of 'dataInput' with 'layoutInput'
        // TODO: how to deal with boundary flags?
        if (*m_pMBLayout != *Input(0)->GetMBLayout()) // this does a deep value-level comparison
            InvalidArgument("%ls %ls operation discovered that %ls %ls operation produced an MB layout that is incompaitble with that of %ls %ls.",
                            NodeName().c_str(), OperationName().c_str(),
                            Input(0)->NodeName().c_str(), Input(0)->OperationName().c_str(),
                            Input(1)->NodeName().c_str(), Input(1)->OperationName().c_str());

        // copy the data from 'dataInput'
        ValueFor(fr).SetValue(Input(0)->ValueFor(fr.WithLayout(Input(0)->GetMBLayout()))); // just propagate through
        // TODO: Once we do in-place, the above must include a copy-to-self check (either here or inside the matrix lib).
    }

    virtual void /*ComputationNodeBase::*/ Validate(bool isFinalValidationPass) override
    {
        Base::Validate(isFinalValidationPass);
        if (isFinalValidationPass && (!Input(0)->HasMBLayout() || !Input(1)->HasMBLayout()))
            RuntimeError("%ls %ls operation requires two inputs that both have an associated MB layout.", NodeName().c_str(), OperationName().c_str());
        m_pMBLayout = Input(1)->GetMBLayout(); // output layout is that of 'layoutInput'
        // Note: We could also enforce that both inputs in fact have different layouts. But maybe there are edge cases where it isn't. Then this just becomes a nop. Also OK.

        SetDims(Input(0));
    }
};

template class ReconcileMBLayoutNode<float>;
template class ReconcileMBLayoutNode<double>;

// -----------------------------------------------------------------------
// RowSliceNode (input)
// this node extracts part of the input by rows as the output
// it has to be continuous segments of rows since each column is treated as one sample
// -----------------------------------------------------------------------

template <class ElemType>
class RowSliceNode : public ComputationNode<ElemType>, public NumInputs<1>
{
    typedef ComputationNode<ElemType> Base;
    UsingComputationNodeMembersBoilerplate;
    static const std::wstring TypeName()
    {
        return L"RowSlice";
    }

public:
    RowSliceNode(DEVICEID_TYPE deviceId, const wstring& name, size_t startIndex = 0, size_t numRows = 0)
        : Base(deviceId, name),
          m_startIndex(startIndex),
          m_sliceHeight(numRows)
    {
    }
    RowSliceNode(const ScriptableObjects::IConfigRecordPtr configp)
        : RowSliceNode(configp->Get(L"deviceId"), L"<placeholder>", configp->Get(L"startIndex"), configp->Get(L"numRows"))
    {
        AttachInputs(configp, this->GetExpectedNumInputs());
    }

    virtual void CopyTo(ComputationNodeBasePtr nodeP, const std::wstring& newName, const CopyNodeFlags flags) const override
    {
        Base::CopyTo(nodeP, newName, flags);
        auto node = dynamic_pointer_cast<RowSliceNode<ElemType>>(nodeP);

        node->m_startIndex = m_startIndex;
        node->m_sliceHeight = m_sliceHeight;
    }

    virtual void Save(File& fstream) const override
    {
        Base::Save(fstream);
        fstream << m_startIndex << m_sliceHeight;
    }

    virtual void Load(File& fstream, size_t modelVersion) override
    {
        Base::Load(fstream, modelVersion);
        fstream >> m_startIndex >> m_sliceHeight;
    }

    virtual void /*ComputationNode::*/ BackpropTo(const size_t /*inputIndex*/, const FrameRange& fr) override
    {
        Input(0)->GradientFor(fr).AddToRowSliceValuesOf(GradientFor(fr), m_startIndex, m_sliceHeight);
    }

    virtual bool OutputUsedInComputingInputNodesGradients() const override
    {
        // The RowSliceNode does not require its output value for computing
        // the gradients of its input nodes
        return false;
    }

    virtual bool InputUsedInComputingInputNodesGradients(size_t childIndex) const override
    {
        // The RowSliceNode does not require any of it's input's values for computing
        // the gradients of its input nodes
        UNREFERENCED_PARAMETER(childIndex);
        return false;
    }

    virtual void /*ComputationNode::*/ ForwardProp(const FrameRange& fr) override
    {
        ValueFor(fr).AssignRowSliceValuesOf(Input(0)->ValueFor(fr), m_startIndex, m_sliceHeight);
    }

    virtual void /*ComputationNodeBase::*/ Validate(bool isFinalValidationPass) override
    {
        Base::Validate(isFinalValidationPass);
        InferMBLayoutFromInputsForStandardCase();

        if (isFinalValidationPass && Input(0)->GetSampleMatrixNumRows() < m_startIndex + m_sliceHeight)
            RuntimeError("%ls %ls operation: m_startIndex + m_sliceHeight exceeds number of rows in the input.", NodeName().c_str(), OperationName().c_str());

        // RowSlice cannot slice tensors.
        // TODO: Create a TensorSlice operation, or just Slice.
        if (isFinalValidationPass && Input(0)->HasSampleLayout() && !Input(0)->GetSampleLayout().IsVectorStoredAsImage() // legacy
            )
            RuntimeError("%ls %ls operation: Input must be a vector, tensor shape [%s] not allowed.", NodeName().c_str(), OperationName().c_str(), string(Input(0)->GetSampleLayout()).c_str());
        SetDims(TensorShape(m_sliceHeight), HasMBLayout());
    }

private:
    size_t m_startIndex, m_sliceHeight;
};

template class RowSliceNode<float>;
template class RowSliceNode<double>;

// -----------------------------------------------------------------------
// RowStackNode (input0, input1, ...)
// stacks multiple inputs on top of each other
// -----------------------------------------------------------------------

template <class ElemType>
class RowStackNode : public ComputationNode<ElemType> // note: not deriving from NumInputs<> like most other nodes, because this one takes a variable number of inputs
{
    typedef ComputationNode<ElemType> Base;
    UsingComputationNodeMembersBoilerplate;
    static const std::wstring TypeName()
    {
        return L"RowStack";
    }

public:
    DeclareConstructorFromConfig(RowStackNode);
    RowStackNode(DEVICEID_TYPE deviceId, const wstring& name)
        : Base(deviceId, name)
    {
    }

    virtual void CopyTo(ComputationNodeBasePtr nodeP, const std::wstring& newName, const CopyNodeFlags flags) const override
    {
        Base::CopyTo(nodeP, newName, flags);
        if (flags & CopyNodeFlags::copyNodeChildren)
        {
            auto node = dynamic_pointer_cast<RowStackNode<ElemType>>(nodeP);
            node->m_startRowIndices = m_startRowIndices;
        }
    }

    virtual void /*ComputationNode::*/ BackpropTo(const size_t inputIndex, const FrameRange& fr) override
    {
        Input(inputIndex)->GradientFor(fr).AddWithRowSliceValuesOf(GradientFor(fr), m_startRowIndices[inputIndex], Input(inputIndex)->GetSampleMatrixNumRows());
    }

    virtual bool OutputUsedInComputingInputNodesGradients() const override
    {
        // The RowStackNode does not require its output value for computing
        // the gradients of its input nodes
        return false;
    }

    virtual bool InputUsedInComputingInputNodesGradients(size_t childIndex) const override
    {
        // The RowStackNode does not require any of it's input's values for computing
        // the gradients of its input nodes
        UNREFERENCED_PARAMETER(childIndex);
        return false;
    }

    virtual void /*ComputationNode::*/ ForwardProp(const FrameRange& fr) override
    {
        for (size_t inputIndex = 0; inputIndex < GetNumInputs(); inputIndex++)
            ValueFor(fr).AssignToRowSliceValuesOf(Input(inputIndex)->ValueFor(fr), m_startRowIndices[inputIndex], Input(inputIndex)->GetSampleMatrixNumRows());
    }

    virtual void /*ComputationNodeBase::*/ Validate(bool isFinalValidationPass) override
    {
        Base::Validate(isFinalValidationPass);
        InferMBLayoutFromInputsForStandardCase();

        // we must fuse all tensor shapes
        // All dimensions but the last must be the same. (In a future version, we should be able to stack along any given dimension.)
        // Note that trailing ones may be stripped/broadcasting, so we must first pad.
        SmallVector<size_t> dims = Input(0)->GetSampleLayout().GetDims();
        size_t maxRank = 0; // TODO: very similar to DetermineElementwiseTensorRank() except that that one also includes the output
        for (int i = 0; i < GetNumInputs(); i++)
            if (maxRank < GetInputSampleLayout(i).GetRank())
                maxRank = GetInputSampleLayout(i).GetRank();
        dims.resize(maxRank - 1, 1); // pad and/or strip trailing dimension

        // count totalRows and form m_startRowIndices[] array, which is the cumulative sum of matrix heights
        m_startRowIndices.resize(GetNumInputs());
        size_t totalRows = 0;
        size_t totalTrailingDim = 0; // last tensor dimension is what gets stacked up
        for (int i = 0; i < GetNumInputs(); i++)
        {
            m_startRowIndices[i] = totalRows;
            totalRows += Input(i)->GetSampleMatrixNumRows();
            SmallVector<size_t> thisDims = Input(i)->GetSampleLayout().GetDims();
            thisDims.resize(maxRank, 1);         // pad and/or strip trailing dimension
            totalTrailingDim += thisDims.back(); // count total trailing dimensions (that's what we have after stacking)
            thisDims.resize(maxRank - 1);        // verify that dimensions match
            if (dims != thisDims)
                InvalidArgument("%ls %ls operation: Incompatible tensor dimension [%s] for input %ls %ls",
                                NodeName().c_str(), OperationName().c_str(), std::string(Input(i)->GetSampleLayout()).c_str(),
                                Input(i)->NodeName().c_str(), Input(i)->OperationName().c_str());
        }

        // warn that this node will destroy the image size information from the child
        if (Input(0)->HasSampleLayout())
            fprintf(stderr, "WARNING: RowStack operation cannot inherit image size information from its child. Image size info is lost.\n");

        dims.push_back(totalTrailingDim);
        SetDims(TensorShape(dims), HasMBLayout());

        if (totalRows != GetSampleMatrixNumRows())
            LogicError("%ls RowStack operation: Tensor shapes of inputs were not compatible after all?", NodeName().c_str());
    }

private:
    std::vector<size_t> m_startRowIndices; // start row number in the stacked matrix of each input (child) (cumsum of matrix heights)
};

template class RowStackNode<float>;
template class RowStackNode<double>;

// -----------------------------------------------------------------------
// RowRepeatNode (input) -- duplicate row(s) of a matrix multiple times
// -----------------------------------------------------------------------

template <class ElemType>
class RowRepeatNode : public ComputationNode<ElemType>, public NumInputs<1>
{
    typedef ComputationNode<ElemType> Base;
    UsingComputationNodeMembersBoilerplate;
    static const std::wstring TypeName()
    {
        return L"RowRepeat";
    }

public:
    RowRepeatNode(DEVICEID_TYPE deviceId, const wstring& name, size_t numRepeats = 1)
        : Base(deviceId, name),
          m_numRepeat(numRepeats)
    {
    }
    RowRepeatNode(const ScriptableObjects::IConfigRecordPtr configp)
        : RowRepeatNode(configp->Get(L"deviceId"), L"<placeholder>", configp->Get(L"numRepeats"))
    {
        AttachInputs(configp, this->GetExpectedNumInputs());
    }

    virtual void CopyTo(ComputationNodeBasePtr nodeP, const std::wstring& newName, const CopyNodeFlags flags) const override
    {
        Base::CopyTo(nodeP, newName, flags);
        if (flags & CopyNodeFlags::copyNodeValue)
        {
            auto node = dynamic_pointer_cast<RowRepeatNode<ElemType>>(nodeP);
            node->m_numRepeat = m_numRepeat;
        }
    }

    virtual void Save(File& fstream) const override
    {
        Base::Save(fstream);
        fstream << m_numRepeat;
    }

    virtual void Load(File& fstream, size_t modelVersion) override
    {
        Base::Load(fstream, modelVersion);
        fstream >> m_numRepeat;
    }

    virtual void PrintSelfBeforeValidation() const override
    {
        Base::PrintSelfBeforeValidation();
        fprintf(stderr, ", numRepeats=%lu", m_numRepeat);
    }

    virtual void /*ComputationNodeBase::*/ Validate(bool isFinalValidationPass) override
    {
        Base::Validate(isFinalValidationPass);
        InferMBLayoutFromInputsForStandardCase();

        // the trailing dimension gets multiplied
        // TODO: Or should we add an additional dimension?
        SmallVector<size_t> dims = GetInputSampleLayout(0).GetDims();
        dims.back() *= m_numRepeat;

        SetDims(TensorShape(dims), HasMBLayout());
    }

    virtual void /*ComputationNode::*/ ForwardProp(const FrameRange& fr) override
    {
        ValueFor(fr).AssignRepeatOf(Input(0)->ValueFor(fr), m_numRepeat, 1);
    }

    virtual void /*ComputationNode::*/ BackpropTo(const size_t /*inputIndex*/, const FrameRange& fr) override
    {
        Input(0)->GradientFor(fr).AddToRowRepeatValuesOf(GradientFor(fr), m_numRepeat);
    }

    virtual bool OutputUsedInComputingInputNodesGradients() const override
    {
        // The RowRepeatNode does not require its output value for computing
        // the gradients of its input nodes
        return false;
    }

    virtual bool InputUsedInComputingInputNodesGradients(size_t childIndex) const override
    {
        // The RowRepeatNode does not require any of it's input's values for computing
        // the gradients of its input nodes
        UNREFERENCED_PARAMETER(childIndex);
        return false;
    }

private:
    size_t m_numRepeat;
};

template class RowRepeatNode<float>;
template class RowRepeatNode<double>;

/*

notes on tensor operations
==========================

reshaping
---------

 - on dimension index 'dim' and 'tensorShape'
 - tensorShape: a vector of dimensions, e.g. 640:480:3:30 could describe a 1-second RGB video of VGA dimensions at 30 fps
 - 'dim' specifies a specific tensor index
    - dim > 0 is a regular sample index. E.g. for a matrix, dim=1 would be the row dimension, and dim=2 in the above example has dimension 480.
    - dim < 0 denote time indices (recurrent loops). Rank=-1 is the innermost time index.
    - dim = 0 denotes the index of the parallel sequence
       - Since all operations logically operate on a single sequence, i.e. parallel sequences generally cannot be indexed by the user.
       - Exceptions: training criteria, BatchNormalization, ...WithNegativeSamples (we should not need this)
    - I don't like that 'dim' refers to the index of the dimension as well as the number of elements in that dimension. Axis (numpy)?

 - Reshaping:   --these are all implemented in C++ by DeprecatedReshapeNode
    - Reshape(x, tensorShape, beginDim=0, endDim=0)
        - just replaces metadata m_sampleLayout
        - one dimension may be specified as 0 and will be inferred
        - optional beginDim/endDim denote to only replace a sub-range of dims, for implementing ReshapeDimension() and FlattenRank()
        - may not be applied to time; use Permute() or Transpose()
    - ReshapeDimension(x, dim, tensorShape) = Reshape(x, tensorShape, beginDim=dim, endDim=dim+1)
       - reinterprets one dimension as multiple, where the number of elements remains the same
       - one of the new dimensions may be specified as 0 and will be inferred
    - FlattenDimensions(x, dim, num) = Reshape(x, 0, beginDim=dim, endDim=dim+1)
       - replace two or more consecutive dims by a single dim with the same number of elements
    - SplitDimension(x, dim, N) = ReshapeDimension(x, dim, 0:N)
       - splits a dimension into a new tensor dimension, injecting them into a new dimension
       - to split stacked frames into a new time dimension:
         insert new time dim with ReshapeDimension(., -1, 0:1), SplitDimension(., dim, N), Transpose(., dim+1, -1), then Select(., dim+1, 0) away the new time dim
         This would make 4 copies presently. We may need a compound C++ node for now.
       - note: to split into multiple outputs (like tf.split()), use a BrainScript loop with Slice().
 - Slicing   --all implemented in C++ by SliceNode
    - Slice(x, dim, begin, end, stride=1, phase=0)
       - reduces a dim to index range [begin,end)
       - negative bounds specify "from end" (end=0 means end if stride>0, and begin=0 means end if stride<0)
       - also applies to time, e.g.:
          - pick last frame of a sequence (for s2s): Slice(x, -1, -1, 0)    // first -1 is dim and means the time index
          - trim first and last 3 frames of a sequence: Slice(x, -1, 3, -3) // 3 means begin at frame 3, -3 means end is 3rd frame from the end
          - this will update MBLayout
       - the optional stride and phase parameters are for implementing downsampling (stride>1) and reversing (begin=-1, stride=-1)
       - multiple slice operations can be combined by concatenating the spec vector, e.g. Slice(x, dim1:dim2, begin1:begin2, end1:end2)
       - today's RowSlice(begin, num, x) = Slice(x, 1, begin, begin + num)
       - like torch.narrow()
       - can implement TF unpack() and Torch split() as a BrainScript loop with multiple Slice() operations
       - internally implemented by tensor lib opCopy with manipulated m_strides/m_offset
    - Select(x, dim, index) = FlattenDimensions(Slice(x, dim, index, index+1), index > 1 ? index-1 : index, index > 1 ? index : index+1)
       - narrow dim to a single index, then drop the dim. Result will have one dim less.
       - like torch.select()
       - can implement squeezing a dim-1 dim: Select(x, dim:0)
    - Squeeze(x, dim) = Select(x, dim, 0)
 - Splicing:   --all implemented in C++ by SpliceNode
    - Splice(inputs, dim)
       - splice multiple inputs inputs[0]:inputs[1]:... along given dim (=RowStack for vectors)
       - inputs must have identical dimensions except for:
          - the specified dim
          - broadcasting dimensions (e.g. used to implement Pad())
       - one can splice in time
          - e.g. prepend a vector to a time sequence
          - this will create a new MBLayout
       - like tf.concat()
    - Pack(inputs, dim) = ReshapeDimension(Splice(inputs, dim), dim, (0:Length(inputs)) )
       - like splice but creates inserts new dim of dimension Length(inputs)
       - inputs must have identical dimensions for all dims (except for broadcasting)
       - dim can be a time dimension; then a new inner-most time dimension will be inserted
       - like tf.pack()
    - Pad(x, dim, howManyBefore, howManyAfter, with=0) = Splice(Constant(with, tensorShape=1*(dim-1):howManyBefore),  x,  Constant(with, tensorShape=1*(dim-1):howManyAfter), dim)
       - inverse of slice, pad with a constant value
       - dimensions specified relative, can pad at start and end
       - in time: pad neighbor frames
    - Repeat(x, dim, numRepeats) = Splice(x*numRepeats, dim)
       - generalizes CNTK RowRepeat(x, numRepeats) = Repeat(x, 1, numRepeats)
       - to repeat multiple, specify vectors, e.g. Repeat(x, dim1:dim2, numRepeats1:numRepeats2)
       - like tf.tile() and Matlab's repmat()
 - Transposition (permuting dims):   --implemented in C++ by PermuteDimensionsNode
    - PermuteDimensionsOf(x, dim1:dim2:...:dimN)
       - dims are rotated to dim2:dim3:...:dimN:dim1; other dims remain untouched
         To rotate the other way round, specify them in opposite order.
         We specify it this way to be able to reference the time dimension without having to know the rank of the m_sampleLayout.
       - time dims must have a constant duration for all items in the minibatch
       - internally implemented with tensor lib by shuffling dimensions with their strides  --TODO: check if TensorShape optimization is still correct
    - Transpose(x, dim1, dim2) = PermuteDimensions(x, dim1:dim2)
       - any two dimensions; including time (must have constant duration)
       - like torch.transpose()
 - Re-indexing:   --implemented by ReindexRankNode and SliceNode
    - ReindexDimension(x, dim, indexVector)
       - splice x[..., indexVector[0], ...], x[..., indexVector[1], ...], etc. with indexVector[.] at given dim
       - indexVector must be invertible if it is intended to backpropagate through this node
    - DownsampleDimension(x, dim, n, phase=0) = Slice(x, dim, 0, 0, stride=n)
       - select every n-th element, starting with index 'phase'
       - time dims allowed. Phase is then a modulus w.r.t. where a sequence is inside the minibatch (may require a ReconcileLayout() before to match layouts)
    - ReverseDimension(x, dim) = Slice(x, dim, -1, 0, stride=-1)
       - reverses the direction of a dim
       - when applied to time dims, this creates a new layout (which is also flipped)

 - misc.:
    - note: much would look more natural if we had OO syntax, e.g. x.Slice(dim, begin, end).FlattenDimensions(...)
      Could be done by exposing all methods on ComputationNode... not currently feasible with BrainScript, but e.g. with Python bindings
    - torch.unfold (dim, size, step)
       - create a convolution matrix (stride magic)
    - CyclicallyPermuteRank(x, dim, step)
       - rotates indices
       - also applies to time dimensions
    - duplicate elements
    - Gather
       - from Torch and TF
    - TF also has:
       - 'gather': reindexing
       - 'dynamic_partition', 'dynamic_stitch'
    - Torch:
       - expand (dim, range): broadcasts dimension 'dim' as a new dimension with 'range'. Not needed I think.
       - repeatTensor: like tile but with weird reshaping
       - squeeze: removes all singleton dimensions, or a specific one. We can remove a specific one with Select().
    - TODO:
       - give names to dimensions?
       - do we want to allow time offsets in layouts?

reductions
----------

 - ReduceSum
    - sum over all elements of a dimension, or over time
 - ReduceMax
    - max
 - ReduceMean
    - av
 - ArgMax, ArgMin
    - we already have that somewhere, for evaluation
 - All, Any
    - logical test --must be done over sequences
 - TF also has:
    - reduce_prod, reduce_min
    - segment_sum etc.; we use sequences
    - listdiff
    - where: indices of 'true' values  -> 2D tensor of coordinates
    - unique (1D only)
    - edit_distance
    - invert_permutation: invert a permutation index vector
    - top_k

convolutions
------------

 - convolution
    - convolution with filter
    - max pool (=convolution with weights 1 and max reduction)
    - av pool (=convolution with uniform filter)
 - also in time: by specifying more filter dimensions [TODO]
    - tricky bit: boundaries; may need expansion or reduction of sequences

element-wise operations
-----------------------

 - PlusNode, MinusNode, ElementTimes
 - with broadcasting, these implement:
    - PlusNode with bias, PlusNode for images
    - 1-x
    - ScaleNode, RowElementTimes, ColumnElementTimes
 - elementwise nonlinearities as usual  [TODO: complete them]
 - logical ops (can be done by comparison ops actually)
 - Clamp
    - bounds are passed as 'Const'
 - TF: in_top_k
 - Torch performs these ops (e.g. add) as vector, without broadcasting
    - e.g. max reduces, while cmax does not. Our solution is better... really? How to specify reduce?

gradient operations
-------------------

 - TF: are nodes, e.g. clip_by_value
    - input should be parameters as well, so they can be computed
 - need a node to stop gradient propagation?
 - can we use nodes to specify things like AdaGrad and momentum?

debugging
---------

 - node that prints activations
 - node that prints mean/var of gradients

other
-----

 - per-node learning rate: can specify additional parameter for each node? Maybe fold with updateLearnableParameter?
 - give dimensions a name?
 - can we interleave variable-length ones? Concat into a single dimensions, using strides?

 */
} } }
