    struct Node {
        void* data;
        Node* left;
        Node* right;
        Node* parent;
        bool color; // 0 = black, 1 = red
    };
class RBTree_t {
public:


protected:
    Node* root;
    /**
     * | 返回值   | 语义                          |
| ----- | --------------------------- |
| `< 0` | **a < b**                   |
| `= 0` | **a == b**（视为红黑树中的“重复 key”） |
| `> 0` | **a > b**                   |

     */
    int (*cmp)(const void*, const void*);//必须定义全序关系


    // --- rotations ---
    Node* left_rotate(Node* x);
    Node* right_rotate(Node* x);


    // --- fix insert ---
    void fix_insert(Node* n);


    // --- find replacement for remove ---
    static Node* subtree_min(Node* x);
    void fix_remove(Node* x, Node* parent);
    static Node* subtree_max(Node* x);
    static Node* successor(Node* x);
public:
    RBTree_t(int (*cmp_func)(const void*, const void*));
    Node* search(void* key);
    int insert(void* data);
    int remove(void* key);
};