# Custom Memory Allocator in C
*A hybrid memory allocator implementing First-Fit, Next-Fit, Best-Fit, Worst-Fit, and Buddy Allocation over a fixed-size memory pool.*

---

## Overview

This project implements a **complete dynamic memory allocator in C**, built entirely from scratch **without using `malloc()` or `free()`**.  
It simulates a user-space heap using a **4 KB memory pool**, supporting both **general-purpose allocation strategies** and a **binary buddy allocator**.

---


## General-Purpose Allocation Strategies

The allocator supports four classical dynamic allocation algorithms:

### **1. First Fit**
- Scans free list from the beginning  
- Selects the *first* block large enough to satisfy the request  

### **2. Next Fit**
- Similar to First Fit  
- But continues search from the *last allocation point*  

### **3. Best Fit**
- Searches entire free list  
- Selects the block with the **smallest size ≥ requested size**  
- Minimizes internal fragmentation  

### **4. Worst Fit**
- Selects the **largest available free block**
- Reduces fragmentation by preserving medium-size blocks

---

### Shared Features of All Four Strategies
- Maintain a **doubly-linked free list** sorted by address  
- Support **block splitting**  
- Automatic **coalescing** (merge neighboring free blocks) during `free()`  
- Maintain robust metadata for safety and consistency

---

## Buddy Allocator (Binary Buddy System)

The allocator also implements the **binary buddy system** inside the same 4 KB pool.

### **Key Features**
- Allocates memory in **power-of-two block sizes**
- Supports orders:
  - **0 to 12**
  - Maximum block size = **4096 bytes**
- Enables **fast splitting** of large blocks into smaller ones
- Performs **efficient buddy merging** on free:
  - Uses bitwise XOR to locate buddy blocks  
  - Merges recursively until the highest possible order is reached

### **Why Buddy Allocation?**
- Very fast: splitting & merging are O(1)
- Simplifies fragmentation management
- Ideal when allocation sizes are predictable or power-of-two aligned

---

## Coexistence of Both Systems

Both allocation systems—general-purpose allocators and the buddy allocator—share the same memory pool.

To ensure correct deallocation:
- Every block stores metadata including:
  - Size
  - Free/allocated flag
  - Magic number (safety)
  - **Buddy order (if allocated by buddy system)**  
- During `my_free()`, the allocator automatically detects:
  - Whether a block belongs to buddy allocator or general allocator
  - Which free list to insert it into
  - Whether merging is required

This allows **seamless hybrid allocation** in a unified memory pool.

---


