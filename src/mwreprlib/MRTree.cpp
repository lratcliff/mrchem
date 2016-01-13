#include "MRTree.h"
#include "HilbertIterator.h"

using namespace std;

/** MRTree constructor.
 *  Requires a world box and polynomial order. */
template<int D>
MRTree<D>::MRTree(const BoundingBox<D> &box)
        : rank(node_group.rank()),
          nThreads(omp_get_max_threads()),
          name("nn"),
          nNodes(0),
          rootBox(box) {
    this->nodesAtDepth.push_back(0);
    allocNodeCounters();

#ifdef OPENMP
    omp_init_lock(&tree_lock);
#endif
}

/** MRTree copy constructor.
 *  Gets world box and polynomial order from another tree. */
template<int D>
MRTree<D>::MRTree(const MRTree<D> &tree)
        : rank(node_group.rank()),
          nThreads(omp_get_max_threads()),
          name("nn"),
          nNodes(0),
          rootBox(tree.rootBox) {
    this->nodesAtDepth.push_back(0);
    allocNodeCounters();

#ifdef OPENMP
    omp_init_lock(&tree_lock);
#endif
}

template<int D>
MRTree<D>::~MRTree() {
    this->endNodeTable.clear();
    if (this->nNodes != 0) {
        MSG_ERROR("Node count != 0 -> " << this->nNodes);
    }
    if (this->nodesAtDepth.size() != 1) {
        MSG_ERROR("Nodes at depth != 1 -> " << this->nodesAtDepth.size());
    }
    if (this->nodesAtDepth[0] != 0) {
        MSG_ERROR("Nodes at depth 0 != 0 -> " << this->nodesAtDepth[0]);
    }
    deleteNodeCounters();

#ifdef OPENMP
    omp_destroy_lock(&tree_lock);
#endif
}

template<int D>
void MRTree<D>::allocNodeCounters() {
    this->nGenNodes = new int[this->nThreads];
    this->nAllocGenNodes = new int[this->nThreads];
    for (int i = 0; i < this->nThreads; i++) {
        this->nGenNodes[i] = 0;
        this->nAllocGenNodes[i] = 0;
    }
}

template<int D>
void MRTree<D>::deleteNodeCounters() {
    delete[] this->nGenNodes;
    delete[] this->nAllocGenNodes;
}

/** Split nodes according to a list of NodeIndices.
  *
  * Given a list of NodeIndices to split, this routine creates the new children
  * nodes. The newly born (local) children nodes are collected in a MRNodeVector.
  * Children nodes are by default given the rank of their parent.*/
template<int D>
void MRTree<D>::splitNodes(const NodeIndexSet &idxSet, MRNodeVector *nVec) {
    NOT_IMPLEMENTED_ABORT;
//    typename set<const NodeIndex<D> *>::iterator it;
//    for (it = idxSet.begin(); it != idxSet.end(); it++) {
//        MRNode<D> &node = getNode(**it);
//        node.createChildren();
//        if (nVec != 0) {
//            for (int i = 0; i < node.getNChildren(); i++) {
//                MRNode<D> *child = &node.getMRChild(i);
//                nVec->push_back(child);
//            }
//        }
//    }
}

/** Increment node counters for non-GenNodes. This routine is not thread
  * safe, and must NEVER be called outside a critical region in parallel.
  * It's way. way too expensive to lock the tree, so don't even think
  * about it. */
template<int D>
void MRTree<D>::incrementNodeCount(int scale) {
    int depth = scale - getRootScale();
    assert(depth >= 0);
    int n = this->nodesAtDepth.size() - 1;
    if (depth > n) {
        for (int i = 0; i < depth - n; i++) {
            this->nodesAtDepth.push_back(0);
        }
    }
    this->nodesAtDepth[depth]++;
    this->nNodes++;
}

/** Decrement node counters for non-GenNodes. This routine is not thread
  * safe, and must NEVER be called outside a critical region in parallel.
  * It's way. way too expensive to lock the tree, so don't even think
  * about it. */
template<int D>
void MRTree<D>::decrementNodeCount(int scale) {
    int depth = scale - getRootScale();
    assert(depth >= 0);
    assert(depth < this->nodesAtDepth.size());
    this->nodesAtDepth[depth]--;
    assert(this->nodesAtDepth[depth] >= 0);
    if (this->nodesAtDepth[depth] == 0 and this->nodesAtDepth.size() > 1) {
        this->nodesAtDepth.pop_back();
    }
    this->nNodes--;
    assert(this->nNodes >= 0);
}

/** Update GenNode counts in a safe way. Since GenNodes are created on the
  * fly, we cannot control when to update the node counters without locking
  * the whole tree. Therefore GenNodes update thread-private counters, which
  * get merged with the correct global counters in xxxNodes[0]. This method
  * should be called outside of the parallel region for performance reasons. */
template<int D>
void MRTree<D>::updateGenNodeCounts() {
    lockTree();
    for (int i = 1; i < this->nThreads; i++) {
        this->nGenNodes[0] += this->nGenNodes[i];
        this->nAllocGenNodes[0] += this->nAllocGenNodes[i];
        this->nGenNodes[i] = 0;
        this->nAllocGenNodes[i] = 0;
    }
    assert(this->nGenNodes[0] >= 0);
    assert(this->nAllocGenNodes[0] >= 0);
    unlockTree();
}

/** Adds a GenNode to the count. */
template<int D>
void MRTree<D>::incrementGenNodeCount() {
    int n = omp_get_thread_num();
    assert(n >= 0);
    assert(n < this->nThreads);
    this->nGenNodes[n]++;
}

/** Removes a GenNode from the count. */
template<int D>
void MRTree<D>::decrementGenNodeCount() {
    int n = omp_get_thread_num();
    assert(n >= 0);
    assert(n < this->nThreads);
    this->nGenNodes[n]--;
}

/** Adds an allocated GenNode to the count. */
template<int D>
void MRTree<D>::incrementAllocGenNodeCount() {
    int n = omp_get_thread_num();
    assert(n >= 0);
    assert(n < this->nThreads);
    this->nAllocGenNodes[n]++;
}

/** Removes an allocated GenNode from the count. */
template<int D>
void MRTree<D>::decrementAllocGenNodeCount() {
    int n = omp_get_thread_num();
    assert(n >= 0);
    assert(n < this->nThreads);
    this->nAllocGenNodes[n]--;
}

/** Get Node count. */
template<int D>
int MRTree<D>::getNNodes(int depth) const {
    if (depth < 0) {
        return this->nNodes;
    }
    if (depth >= this->nodesAtDepth.size()) {
        return 0;
    }
    return this->nodesAtDepth[depth];
}

/** Get allocated GenNode count. Includes an OMP reduction operation. */
template<int D>
int MRTree<D>::getNAllocGenNodes() {
    updateGenNodeCounts();
    return this->nAllocGenNodes[0];
}

/** Get GenNode count. Includes an OMP reduction operation. */
template<int D>
int MRTree<D>::getNGenNodes() {
    updateGenNodeCounts();
    return this->nGenNodes[0];
}

/** Find and return the node with the given NodeIndex, const version.
  *
  * Recursive routine to find and return the node with a given NodeIndex.
  * This routine returns the appropriate ProjectedNode, or a NULL pointer if
  * the node does not exist, or if it is a GenNode. Recursion starts at the
  * appropriate rootNode. */
template<int D>
const MRNode<D>* MRTree<D>::findNode(const NodeIndex<D> &idx) const {
    const MRNode<D> &root = getRootBox().getNode(idx);
    assert(root.isAncestor(idx));
    return root.retrieveNodeNoGen(idx);
}

/** Find and return the node with the given NodeIndex.
  *
  * Recursive routine to find and return the node with a given NodeIndex.
  * This routine returns the appropriate ProjectedNode, or a NULL pointer if
  * the node does not exist, or if it is a GenNode. Recursion starts at the
  * appropriate rootNode. */
template<int D>
MRNode<D>* MRTree<D>::findNode(const NodeIndex<D> &idx) {
    MRNode<D> &root = this->rootBox.getNode(idx);
    assert(root.isAncestor(idx));
    return root.retrieveNodeNoGen(idx);
}

/** Find and return the node with the given NodeIndex.
  *
  * This routine ALWAYS returns the node you ask for, and will generate nodes
  * that does not exist. Recursion starts at the appropriate rootNode and
  * decends from this.*/
template<int D>
MRNode<D>& MRTree<D>::getNode(const NodeIndex<D> &idx) {
    MRNode<D> &root = getRootBox().getNode(idx);
    assert(root.isAncestor(idx));
    return *root.retrieveNode(idx);
}

/** Find and return the node with the given NodeIndex.
  *
  * This routine returns the ProjectedNode you ask for, or the EndNode on
  * the path to the requested node, and will never create or return GenNodes.
  * Recursion starts at the appropriate rootNode and decends from this. */
template<int D>
MRNode<D>& MRTree<D>::getNodeOrEndNode(const NodeIndex<D> &idx) {
    MRNode<D> &root = getRootBox().getNode(idx);
    assert(root.isAncestor(idx));
    return *root.retrieveNodeOrEndNode(idx);
}

/** Find and return the node with the given NodeIndex.
  *
  * This routine returns the ProjectedNode you ask for, or the EndNode on
  * the path to the requested node, and will never create or return GenNodes.
  * Recursion starts at the appropriate rootNode and decends from this. */
template<int D>
const MRNode<D>& MRTree<D>::getNodeOrEndNode(const NodeIndex<D> &idx) const {
    const MRNode<D> &root = getRootBox().getNode(idx);
    assert(root.isAncestor(idx));
    return *root.retrieveNodeOrEndNode(idx);
}

/** Find and return the node at a given depth that contains a given coordinate.
  *
  * This routine ALWAYS returns the node you ask for, and will generate nodes
  * that does not exist. Recursion starts at the appropriate rootNode and
  * decends from this. */
template<int D>
MRNode<D>& MRTree<D>::getNode(const double *r, int depth) {
    MRNode<D> &root = getRootBox().getNode(r);
    if (depth >= 0) {
        return *root.retrieveNode(r, depth);
    } else {
        return *root.retrieveNodeOrEndNode(r, depth);
    }
}

/** Find and return the node at a given depth that contains a given coordinate.
  *
  * This routine returns the ProjectedNode you ask for, or the EndNode on
  * the path to the requested node, and will never create or return GenNodes.
  * Recursion starts at the appropriate rootNode and decends from this. */
template<int D>
MRNode<D>& MRTree<D>::getNodeOrEndNode(const double *r, int depth) {
    MRNode<D> &root = getRootBox().getNode(r);
    return *root.retrieveNodeOrEndNode(r, depth);
}

/** Find and return the node at a given depth that contains a given coordinate.
  *
  * This routine returns the ProjectedNode you ask for, or the EndNode on
  * the path to the requested node, and will never create or return GenNodes.
  * Recursion starts at the appropriate rootNode and decends from this. */
template<int D>
const MRNode<D>& MRTree<D>::getNodeOrEndNode(const double *r, int depth) const {
    const MRNode<D> &root = getRootBox().getNode(r);
    return *root.retrieveNodeOrEndNode(r, depth);
}

/** Traverse nodeTable and find all nodes of different rankId. */
template<int D>
void MRTree<D>::findMissingNodes(MRNodeVector &nodeTable,
                                 set<MRNode<D> *> &missing) {
    NOT_IMPLEMENTED_ABORT;
//    for (unsigned int i = 0; i < nodeTable.size(); i++) {
//        MRNode<D> &node = *nodeTable[i];
//        if (not node.hasCoefs()) {
//            assert(node.isForeign());
//            missing.insert(&node);
//        }
//    }
}

/** Traverse nodeTable and find all nodes with parent of different rankId. */
template<int D>
void MRTree<D>::findMissingParents(MRNodeVector &nodeTable,
                                   set<MRNode<D> *> &missing) {
    NOT_IMPLEMENTED_ABORT;
//    for (unsigned int i = 0; i < nodeTable.size(); i++) {
//        MWNode<D> &node = *nodeTable[i];
//        if (node.isRoot()) {
//            continue;
//        }
//        MWNode<D> &parent = node.getMWParent();
//        if (not parent.hasCoefs()) {
//            assert(this->getRankId() != parent.getRankId());
//            missing.insert(&parent);
//        }
//    }
}

/** Traverse nodeTable and find all nodes with children of different rankId. */
template<int D>
void MRTree<D>::findMissingChildren(
        MRNodeVector &nodeTable, set<MRNode<D> *> &missing) {
    NOT_IMPLEMENTED_ABORT;
//    for (unsigned int i = 0; i < nodeTable.size(); i++) {
//        MRNode<D> &node = *nodeTable[i];
//        if (node.isEndNode()) {
//            continue;
//        }
//        for (int n = 0; n < node.getNChildren(); n++) {
//            MRNode<D> &child = node.getMRChild(n);
//            if (not child.hasCoefs()) {
//                assert(this->getRankId() != child.getRankId());
//                missing.insert(&child);
//            }
//        }
//    }
}

/** Traverse tree along the Hilbert path and find nodes of any rankId.
  * Returns one nodeVector for the whole tree. GenNodes disregarded. */
template<int D>
void MRTree<D>::makeNodeTable(MRNodeVector &nodeTable) {
    NOT_IMPLEMENTED_ABORT;
//    HilbertIterator<D> it(this);
//    while (it.next()) {
//        MWNode<D> &node = it.getNode();
//        if (node.isGenNode()) {
//            continue;
//        }
//        if (not root and node.isRoot()) {
//            continue;
//        }
//        nodeTable.push_back(&node);
//    }
}

/** Traverse tree along the Hilbert path and find nodes of any rankId.
  * Returns one nodeVector per scale. GenNodes disregarded. */
template<int D>
void MRTree<D>::makeNodeTable(std::vector<MRNodeVector > &nodeTable) {
    NOT_IMPLEMENTED_ABORT;
//    HilbertIterator<D> it(this);
//    while (it.next()) {
//        MRNode<D> &node = it.getNode();
//        if (node.isGenNode()) {
//            continue;
//        }
//        int depth = node.getDepth();
//        if (depth + 1 > nodeTable.size()) { // Add one more element
//            nodeTable.push_back(MRNodeVector());
//        }
//        nodeTable[depth].push_back(&node);
//    }
}

/** Traverse tree along the Hilbert path and find nodes of local rankId.
  * Returns one nodeVector for the whole tree. GenNodes disregarded. */
template<int D>
void MRTree<D>::makeLocalNodeTable(MRNodeVector &nodeTable, bool common) {
    NOT_IMPLEMENTED_ABORT;
//    HilbertIterator<D> it(this);
//    while (it.next()) {
//        MRNode<D> &node = it.getNode();
//        if (node.isGenNode()) {
//            continue;
//        }
//        if (node.isLocal() or (node.isCommon() and common)) {
//            nodeTable.push_back(&node);
//        }
//    }
}

/** Traverse tree along the Hilbert path and find nodes of local rankId.
  * Returns one nodeVector per scale. GenNodes disregarded. */
template<int D>
void MRTree<D>::makeLocalNodeTable(std::vector<MRNodeVector > &nodeTable, bool common) {
    NOT_IMPLEMENTED_ABORT;
//    HilbertIterator<D> it(this);
//    while (it.next()) {
//        MRNode<D> &node = it.getNode();
//        if (node.isGenNode()) {
//            continue;
//        }
//        int depth = node.getDepth();
//        if (depth + 1 > nodeTable.size()) { // Add one more element
//            nodeTable.push_back(MRNodeVector());
//        }
//        if (node.isLocal() or (node.isCommon() and common)) {
//            nodeTable[depth].push_back(&node);
//        }
//    }
}

template<int D>
MRNodeVector* MRTree<D>::copyEndNodeTable() {
    NOT_IMPLEMENTED_ABORT;
//    MRNodeVector *nVec = new MRNodeVector;
//    for (int n = 0; n < getNEndNodes(); n++) {
//        MRNode<D> &node = getEndNode(n);
//        nVec->push_back(&node);
//    }
//    return nVec;
}

template<int D>
void MRTree<D>::resetEndNodeTable() {
    clearEndNodeTable();
    HilbertIterator<D> it(this);
    it.setReturnGenNodes(false);
    while (it.next()) {
        MRNode<D> &node = it.getNode();
        if (node.isEndNode()) {
            this->endNodeTable.push_back(&node);
        }
    }
}

template<int D>
int MRTree<D>::countBranchNodes(int depth) {
    NOT_IMPLEMENTED_ABORT;
}

template<int D>
int MRTree<D>::countLeafNodes(int depth) {
    NOT_IMPLEMENTED_ABORT;
//    int nNodes = 0;
//    HilbertIterator<D> it(this);
//    while (it.next()) {
//        MRNode<D> &node = it.getNode();
//        if (node.getDepth() == depth or depth < 0) {
//            if (node.isLeafNode()) {
//                nNodes++;
//            }
//        }
//    }
//    return nNodes;
}

/** Traverse tree and count nodes belonging to this rank. */
template<int D>
int MRTree<D>::countMyNodes(int depth) {
    NOT_IMPLEMENTED_ABORT;
//    HilbertIterator<D> it(this);
//    int count = 0;
//    while (it.next()) {
//        MRNode<D> &node = it.getNode();
//        if (node.isGenNode()) {
//            continue;
//        }
//        if (not node.isForeign()) {
//            count++;
//        }
//    }
//    return count;
}

/** Traverse tree and count nodes with allocated coefficients. */
template<int D>
int MRTree<D>::countAllocNodes(int depth) {
    NOT_IMPLEMENTED_ABORT;
//    HilbertIterator<D> it(this);
//    int count = 0;
//    while (it.next()) {
//        MWNode<D> &node = it.getNode();
//        if (node.isGenNode()) {
//            continue;
//        }
//        if (node.hasCoefs()) {
//            count++;
//        }
//    }
//    return count;
}

/** Print the number of nodes, sorted by depth and MPI rank. */
template<int D>
void MRTree<D>::printNodeRankCount() {
    NOT_IMPLEMENTED_ABORT;
//    int nHosts = node_group.size();
//    int mDepth = getDepth();
//    int count[mDepth + 1][nHosts+1];
//    for (int i = 0; i < mDepth + 1; i++) {
//        for (int j = 0; j < nHosts+1; j++) {
//            count[i][j] = 0;
//        }
//    }
//    HilbertIterator<D> it(this);
//    while(it.next()) {
//        MRNode<D> &node = it.getNode();
//        if (node.isGenNode()) {
//            continue;
//        }
//        int depth = node.getDepth();
//        int rank = node.getRankId();
//        if (rank >= 0) {
//            count[depth][rank+1]++;
//            count[mDepth][rank+1]++;
//        } else {
//            count[depth][0]++;
//            count[mDepth][0]++;
//        }
//    }

//    println(0, endl);
//    printout(0, "   |");
//    for (int j = -1; j < nHosts; j++) {
//        printout(0, setw(6) << j);
//    }
//    println(0, "|" << endl);
//    for (int i = 0; i < mDepth + 1; i++) {
//        if (i == mDepth) {
//            printout(0, endl);
//        }
//        printout(0, setw(3) << i << "|");
//        for (int j = 0; j < nHosts+1; j++) {
//            printout(0, setw(6) << count[i][j]);
//        }
//        println(0, "|");
//    }
//    println(0, endl);
}

template<int D>
void MRTree<D>::distributeNodes(int depth) {
    NOT_IMPLEMENTED_ABORT;
//    MRNodeVector nodeTable;
//    HilbertIterator<D> it(this);
//    it.setReturnGenNodes(false);
//    it.setMaxDepth(depth);
//    while (it.next()) {
//        MRNode<D> &node = it.getNode();
//        if (node.isEndNode() or node.getDepth() == depth) {
//            nodeTable.push_back(&node);
//        }
//    }
//    distributeNodeTags(nodeTable);
//    tagDecendants(nodeTable);
}

/** Tag each node with the rank who owns it. */
template<int D>
void MRTree<D>::distributeNodeTags(MRNodeVector &nodeList) {
    NOT_IMPLEMENTED_ABORT;
//    int start, end;
//    int nNodes = nodeList.size();
//    int nHosts = node_group.size();
//    for (int k = 0; k < nHosts; k++) {
//        get_locale_index_range(k, nNodes, start, end);
//        for (int i = start; i < end; i++) {
//            MRNode<D> &node = *nodeList[i];
//            node.setRankId(k);
//        }
//    }
}

/** Set the rank id (node tag) for all nodes in list. */
template<int D>
void MRTree<D>::tagNodes(MRNodeVector &nodeList, int rank) {
    NOT_IMPLEMENTED_ABORT;
//    for (int i = 0; i < nodeList.size(); i++) {
//        MRNode<D> &node = *nodeList[i];
//        node.setRankId(rank);
//    }
}

template<int D>
void MRTree<D>::tagDecendants(MRNodeVector &nodeList) {
    NOT_IMPLEMENTED_ABORT;
//    int nNodes = nodeList.size();
//    for (int i = 0; i < nNodes; i++) {
//        MRNode<D> &node = *nodeList[i];
//        node.assignDecendantTags(node.getRankId());
//    }
}

/** Traverse tree and remove nodes of foreign rank.
  * Option to keep all endNodes. */
template<int D>
void MRTree<D>::deleteForeign(bool keepEndNodes) {
    NOT_IMPLEMENTED_ABORT;
//    if (not this->isScattered()) {
//        return;
//    }
//    HilbertIterator<D> it(this);

//    while (it.next()) {
//        MWNode<D> &node = it.getNode();
//        if (keepEndNodes and node.isEndNode()) {
//            continue;
//        }
//        if (node.isForeign()) {
//            node.clearCoefs();
//            node.clearNorms();
//        }
//        node.clearRedundancy();
//    }
}

template<int D>
void MRTree<D>::deleteGenerated() {
    for (int n = 0; n < getNEndNodes(); n++) {
        getEndNode(n).deleteGenerated();
    }
}

/** Loop through endNodeTable and recursively clear all GenNode coefficients.
  * Includes a static cast of endNodes from MWNode to FunctionNode*/
template<int D>
void MRTree<D>::clearGenerated() {
    NOT_IMPLEMENTED_ABORT;
//    for (int i = 0; i < this->endNodeTable.size(); i++) {
//        FunctionNode<D> &node = static_cast<FunctionNode<D> &>(*this->endNodeTable[i]);
//        node.clearGenerated();
//    }
}

template<int D>
void MRTree<D>::checkGridOverlap(MRTree<D> &tree) {
    NOT_IMPLEMENTED_ABORT;
//    int overlapA = 0;
//    int overlapB = 0;
//    int onlyA = 0;
//    int onlyB = 0;

//    HilbertIterator<D> itA(this);
//    itA.setReturnGenNodes(false);
//    while (itA.next()) {
//        const NodeIndex<D> &idx = itA.getNode().getNodeIndex();
//        MWNode<D> *nodeB = tree.findNode(idx);
//        if (nodeB != 0) {
//            overlapA++;
//        } else {
//            onlyA++;
//        }
//    }

//    HilbertIterator<D> itB(&tree);
//    itB.setReturnGenNodes(false);
//    while (itB.next()) {
//        const NodeIndex<D> &idx = itB.getNode().getNodeIndex();
//        MWNode<D> *nodeA = this->findNode(idx);
//        if (nodeA != 0) {
//            overlapB++;
//        } else {
//            onlyB++;
//        }
//    }

//    int nodesA = this->getNNodes();
//    int nodesB = tree.getNNodes();

//    if (overlapA != overlapB) {
//        MSG_WARN("Something went wrong, overlaps do not match.");
//    }
//    if (nodesA != (overlapA + onlyA)) {
//        MSG_WARN("Something went wrong, overlaps do not match.");
//    }
//    if (nodesB != (overlapB + onlyB)) {
//        MSG_WARN("Something went wrong, overlaps do not match.");
//    }

//    printout(0, "Overlapping nodes: ");
//    println(0, setw(8) << onlyA << setw(8) << overlapA << setw(8) << onlyB);
}

template<int D>
void MRTree<D>::checkRankOverlap(MRTree<D> &tree) {
    NOT_IMPLEMENTED_ABORT;
//    MatrixXi rankDiff = MatrixXi::Zero(2,11);

//    for (int i = 0; i < 11; i++) {
//        rankDiff(0,i) = -5 + i;
//    }

//    HilbertIterator<D> itA(this);
//    itA.setReturnGenNodes(false);
//    while (itA.next()) {
//        MWNode<D> &nodeA = itA.getNode();
//        MWNode<D> &nodeB = tree.getNodeNoGen(nodeA.getNodeIndex());
//        int rankA = nodeA.getRankId();
//        int rankB = nodeB.getRankId();
//        int diff = rankA - rankB;
//        if (diff <= -5) {
//            diff = 0;
//        } else if (diff >= 5) {
//            diff = 10;
//        } else {
//            diff += 5;
//        }
//        rankDiff(1, diff)++;
//    }

//    HilbertIterator<D> itB(&tree);
//    itB.setReturnGenNodes(false);
//    while (itB.next()) {
//        MWNode<D> &nodeB = itB.getNode();
//        MWNode<D> &nodeA = tree.getNodeNoGen(nodeB.getNodeIndex());
//        if (nodeA.getScale() == nodeB.getScale()) {
//            continue;
//        }
//        int rankA = nodeA.getRankId();
//        int rankB = nodeB.getRankId();
//        int diff = rankA - rankB;
//        if (diff <= -5) {
//            diff = 0;
//        } else if (diff >= 5) {
//            diff = 10;
//        } else {
//            diff += 5;
//        }
//        rankDiff(1, diff)++;
//    }
//    println(0, rankDiff.row(1));
}

template class MRTree<1>;
template class MRTree<2>;
template class MRTree<3>;
