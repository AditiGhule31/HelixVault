#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ==========================================
// 1. DATA STRUCTURE: SINGLY LINKED LIST
// ==========================================
// To track metadata and multiple owners of a file.
typedef struct MetadataNode {
    char owner[50];
    struct MetadataNode* next;
} MetadataNode;

// ==========================================
// 2. STORAGE BLOCK (Hard Disk Representation)
// ==========================================
typedef struct StorageBlock {
    char* content;
    int refCount; // Number of users sharing the same file
    MetadataNode* ownersHead; // Linked list of users who own this file
    char fileHash[64]; // Hash value of the file
    struct StorageBlock* next; // For handling Hash Table collisions (Chaining)
} StorageBlock;

// ==========================================
// 3. STORAGE ENGINE (Hash Map / Hash Table in C)
// ==========================================
#define HASH_TABLE_SIZE 100

StorageBlock* disk[HASH_TABLE_SIZE];
int totalSpaceSaved = 0;
int uniqueFilesCount = 0;

// Custom strdup (as it is not part of standard ANSI C)
char* my_strdup(const char* str) {
    char* dup = (char*)malloc(strlen(str) + 1);
    if(dup) strcpy(dup, str);
    return dup;
}

// Hashing Function: Generates a hash value from content (djb2 algorithm)
unsigned long calculateHash(const char* str) {
    unsigned long hash = 5381;
    int c;
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c; /* hash * 33 + c */
    }
    return hash;
}

// Upload Logic
int processUpload(const char* content, const char* user, char* outHash) {
    // Generate a unique string hash from content
    sprintf(outHash, "%lu", calculateHash(content));
    
    // Table index
    unsigned int index = calculateHash(content) % HASH_TABLE_SIZE;
    
    // Deduplication Logic (Search in Hash Table)
    StorageBlock* curr = disk[index];
    while(curr != NULL) {
        if(strcmp(curr->fileHash, outHash) == 0) {
            // File already exists! Duplicate found
            MetadataNode* newNode = (MetadataNode*)malloc(sizeof(MetadataNode));
            strcpy(newNode->owner, user);
            newNode->next = curr->ownersHead;
            curr->ownersHead = newNode;
            
            curr->refCount++;
            totalSpaceSaved += strlen(content); // Bytes saved
            return 1; // It is a duplicate
        }
        curr = curr->next;
    }
    
    // New file (Not a duplicate)
    StorageBlock* newBlock = (StorageBlock*)malloc(sizeof(StorageBlock));
    newBlock->content = my_strdup(content);
    strcpy(newBlock->fileHash, outHash);
    newBlock->refCount = 1;
    
    newBlock->ownersHead = (MetadataNode*)malloc(sizeof(MetadataNode));
    strcpy(newBlock->ownersHead->owner, user);
    newBlock->ownersHead->next = NULL;
    
    // Insert into Hash Table at 'index'
    newBlock->next = disk[index];
    disk[index] = newBlock;
    
    uniqueFilesCount++;
    return 0; // New file
}

void printStats() {
    printf("\n========== STORAGE ANALYTICS ==========\n");
    printf("Unique Files on Disk : %d\n", uniqueFilesCount);
    printf("Total Space Saved    : %d Bytes\n", totalSpaceSaved);
    printf("=======================================\n");
}


// ==========================================
// 4. DATA STRUCTURE: TRIE (FILE SYSTEM VIEW)
// ==========================================
// Folder names are strings, so children are managed using a Linked List

struct TrieNode; // Forward declaration

typedef struct TrieChild {
    char name[100];
    struct TrieNode* node;
    struct TrieChild* next;
} TrieChild;

typedef struct TrieNode {
    TrieChild* headChild;
    int isFile;
    char fileHash[64]; // To link with Storage engine
} TrieNode;

TrieNode* root = NULL;

void initFileSystem() {
    root = (TrieNode*)malloc(sizeof(TrieNode));
    root->headChild = NULL;
    root->isFile = 0;
}

void insertPath(const char* path, const char* hash) {
    char pathCopy[256];
    strcpy(pathCopy, path);
    
    // String tokenization considering both slashes (Linux & Windows paths supported)
    char* token = strtok(pathCopy, "/\\");
    TrieNode* curr = root;
    
    while(token != NULL) {
        TrieChild* child = curr->headChild;
        TrieChild* found = NULL;
        
        // Check if this folder already exists
        while(child != NULL) {
            if(strcmp(child->name, token) == 0) {
                found = child;
                break;
            }
            child = child->next;
        }
        
        // If folder not found, create a new one
        if(found == NULL) {
            found = (TrieChild*)malloc(sizeof(TrieChild));
            strcpy(found->name, token);
            found->node = (TrieNode*)malloc(sizeof(TrieNode));
            found->node->headChild = NULL;
            found->node->isFile = 0;
            
            // Add to Trie node
            found->next = curr->headChild;
            curr->headChild = found;
        }
        
        curr = found->node;
        token = strtok(NULL, "/\\");
    }
    
    // The last node becomes a file
    curr->isFile = 1;
    strcpy(curr->fileHash, hash);
}

// Recursive function to print the Tree
void printTree(TrieNode* node, const char* prefix) {
    TrieChild* child = node->headChild;
    while(child != NULL) {
        printf("%s|-- %s", prefix, child->name);
        if(child->node->isFile) {
            printf(" (Linked Hash: %s)\n", child->node->fileHash);
        } else {
            printf("\n");
            char newPrefix[256];
            sprintf(newPrefix, "%s    ", prefix);
            printTree(child->node, newPrefix);
        }
        child = child->next;
    }
}

void displayHierarchy() {
    printf("\n========== VIRTUAL FILE SYSTEM ==========\n");
    printf("root\n");
    printTree(root, "  ");
    printf("=========================================\n");
}


// ==========================================
// 5. MAIN MENU INTERFACE
// ==========================================
int main() {
    // Initialize Hash Table array to NULL
    for(int i = 0; i < HASH_TABLE_SIZE; i++) {
        disk[i] = NULL;
    }
    initFileSystem();

    char inputBuffer[256];
    int choice;
    while (1) {
        printf("\n--- CLOUD STORAGE MANAGER (Mini AWS S3) in C ---\n");
        printf("1. Upload File\n");
        printf("2. View File System Hierarchy (Trie)\n");
        printf("3. View Storage Analytics (Deduplication Info)\n");
        printf("4. Exit\n");
        printf("Enter choice: ");
        
        if (!fgets(inputBuffer, sizeof(inputBuffer), stdin)) break;
        if (sscanf(inputBuffer, "%d", &choice) != 1) {
            printf("Invalid input. Please enter a number.\n");
            continue;
        }

        if (choice == 1) {
            char user[50], path[256], content[1024], outHash[64];
            
            printf("\nEnter User Name: ");
            fgets(inputBuffer, sizeof(inputBuffer), stdin);
            sscanf(inputBuffer, "%49s", user);
            
            printf("Enter File Path (e.g. %s/photos/pic.png): ", user);
            fgets(inputBuffer, sizeof(inputBuffer), stdin);
            sscanf(inputBuffer, "%255s", path);
            
            printf("Enter File Content (type a sentence): ");
            fgets(content, 1024, stdin);
            
            // Remove newline from fgets
            size_t len = strlen(content);
            if(len > 0 && content[len-1] == '\n') content[len-1] = '\0';

            int isDuplicate = processUpload(content, user, outHash);
            insertPath(path, outHash);

            printf("\n[UPLOADING...]\n");
            if (isDuplicate) {
                printf("[SUCCESS] File Uploaded!\n");
                printf(">>>  DEDUPLICATION TRIGGERED: This content was already uploaded by someone.\n");
                printf(">>> Space saved! File linked securely to existing storage block.\n");
            } else {
                printf("[SUCCESS] File Uploaded!\n");
                printf(">>> UNIQUE FILE: New block allocated on storage drive.\n");
            }
        } 
        else if (choice == 2) {
            displayHierarchy();
        } 
        else if (choice == 3) {
            printStats();
        } 
        else if (choice == 4) {
            printf("Exiting system. Bye!\n");
            break;
        } 
        else {
            printf("Invalid choice. Try again.\n");
        }
    }

    return 0;
}
