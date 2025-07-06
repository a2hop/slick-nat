#!/bin/bash

validate_batch_file() {
    local file="$1"
    local line_num=0
    local errors=0
    
    if [ ! -f "$file" ]; then
        echo "Error: File $file not found"
        return 1
    fi
    
    echo "Validating batch file: $file"
    
    while IFS= read -r line; do
        line_num=$((line_num + 1))
        
        # Skip empty lines and comments
        if [ -z "$line" ] || [[ "$line" =~ ^[[:space:]]*# ]]; then
            continue
        fi
        
        # Parse line for different commands
        if [[ "$line" =~ ^(add|del|drop)[[:space:]]+([^[:space:]]+)([[:space:]]+([^[:space:]]+))?([[:space:]]+([^[:space:]]+))?$ ]]; then
            local cmd="${BASH_REMATCH[1]}"
            local interface="${BASH_REMATCH[2]}"
            local internal="${BASH_REMATCH[4]}"
            local external="${BASH_REMATCH[6]}"
            
            # Validate command
            if [ "$cmd" != "add" ] && [ "$cmd" != "del" ] && [ "$cmd" != "drop" ]; then
                echo "Line $line_num: Invalid command '$cmd'"
                errors=$((errors + 1))
                continue
            fi
            
            # Validate add command has both prefixes
            if [ "$cmd" = "add" ] && ([ -z "$internal" ] || [ -z "$external" ]); then
                echo "Line $line_num: 'add' command requires both internal and external prefixes"
                errors=$((errors + 1))
                continue
            fi
            
            # Validate del command has internal prefix
            if [ "$cmd" = "del" ] && [ -z "$internal" ]; then
                echo "Line $line_num: 'del' command requires internal prefix"
                errors=$((errors + 1))
                continue
            fi
            
            # Validate drop command (only needs interface or --all)
            if [ "$cmd" = "drop" ]; then
                if [ "$interface" != "--all" ] && [ -z "$interface" ]; then
                    echo "Line $line_num: 'drop' command requires interface name or --all"
                    errors=$((errors + 1))
                    continue
                fi
                # Drop doesn't need additional validation
                continue
            fi
            
            # Validate IPv6 prefix format (basic check) for add/del commands
            if [[ ! "$internal" =~ ^[0-9a-fA-F:]+/[0-9]+$ ]]; then
                echo "Line $line_num: Invalid internal prefix format '$internal'"
                errors=$((errors + 1))
                continue
            fi
            
            if [ "$cmd" = "add" ] && [[ ! "$external" =~ ^[0-9a-fA-F:]+/[0-9]+$ ]]; then
                echo "Line $line_num: Invalid external prefix format '$external'"
                errors=$((errors + 1))
                continue
            fi
            
        else
            echo "Line $line_num: Invalid syntax - '$line'"
            errors=$((errors + 1))
        fi
    done < "$file"
    
    if [ $errors -eq 0 ]; then
        echo "Validation passed: $line_num lines processed, 0 errors"
        return 0
    else
        echo "Validation failed: $errors errors found"
        return 1
    fi
}


add_batch() {
    local file="$1"
    
    if [ -z "$file" ]; then
        echo "Usage: $0 add-batch <file>"
        echo ""
        echo "File format (one operation per line):"
        echo "  add <interface> <internal_prefix/len> <external_prefix/len>"
        echo "  del <interface> <internal_prefix/len>"
        echo "  drop <interface>    - Drop all mappings for interface"
        echo "  drop --all         - Drop all mappings"
        echo "  # Comments are ignored"
        echo ""
        echo "Example:"
        echo "  add eth0 2001:db8:internal:1::/64 2001:db8:external:1::/64"
        echo "  add eth0 2001:db8:internal:2::/64 2001:db8:external:2::/64"
        echo "  del eth0 2001:db8:internal:3::/64"
        echo "  drop eth1"
        echo "  drop --all"
        return 1
    fi
    
    check_module
    check_container_permissions
    
    if [ ! -f "$PROC_BATCH_FILE" ]; then
        echo "Error: Batch interface not available"
        echo "This may indicate an older version of the kernel module"
        return 1
    fi
    
    # Validate file first
    if ! validate_batch_file "$file"; then
        echo "Batch file validation failed. Please fix errors and try again."
        return 1
    fi
    
    echo "Processing batch file: $file"
    
    # Count non-comment lines for progress
    local total_lines=$(grep -v '^\s*#' "$file" | grep -v '^\s*$' | wc -l)
    echo "Total operations to process: $total_lines"
    
    # Process the batch
    if cat "$file" > "$PROC_BATCH_FILE" 2>/dev/null; then
        echo "Batch operation completed successfully"
        echo "Use '$0 status' to verify the mappings"
        return 0
    else
        echo "Error: Batch operation failed"
        echo "Check file format and permissions"
        if is_container; then
            echo "Container may need additional privileges (see documentation)"
        fi
        return 1
    fi
}

del_batch() {
    local file="$1"
    
    if [ -z "$file" ]; then
        echo "Usage: $0 del-batch <file>"
        echo ""
        echo "File format (one operation per line):"
        echo "  del <interface> <internal_prefix/len>"
        echo "  drop <interface>    - Drop all mappings for interface"
        echo "  drop --all         - Drop all mappings"
        echo "  # Comments are ignored"
        echo ""
        echo "Example:"
        echo "  del eth0 2001:db8:internal:1::/64"
        echo "  del eth0 2001:db8:internal:2::/64"
        echo "  drop eth1"
        echo "  drop --all"
        return 1
    fi
    
    check_module
    check_container_permissions
    
    if [ ! -f "$PROC_BATCH_FILE" ]; then
        echo "Error: Batch interface not available"
        echo "This may indicate an older version of the kernel module"
        return 1
    fi
    
    # Validate file first (allow only del and drop operations)
    if ! validate_batch_file "$file"; then
        echo "Batch file validation failed. Please fix errors and try again."
        return 1
    fi
    
    # Check that file contains only del and drop operations
    if grep -v '^\s*#' "$file" | grep -v '^\s*$' | grep -v -E '^(del|drop) ' | head -1 > /dev/null; then
        echo "Error: del-batch file can only contain 'del' and 'drop' operations"
        return 1
    fi
    
    echo "Processing batch delete file: $file"
    
    # Count non-comment lines for progress
    local total_lines=$(grep -v '^\s*#' "$file" | grep -v '^\s*$' | wc -l)
    echo "Total delete operations to process: $total_lines"
    
    # Process the batch
    if cat "$file" > "$PROC_BATCH_FILE" 2>/dev/null; then
        echo "Batch delete operation completed successfully"
        echo "Use '$0 status' to verify the mappings were removed"
        return 0
    else
        echo "Error: Batch delete operation failed"
        echo "Check file format and permissions"
        if is_container; then
            echo "Container may need additional privileges (see documentation)"
        fi
        return 1
    fi
}

create_batch_template() {
    local file="$1"
    
    if [ -z "$file" ]; then
        echo "Usage: $0 create-template <file>"
        echo ""
        echo "Creates a template batch file for editing"
        return 1
    fi
    
    if [ -f "$file" ]; then
        echo "Error: File $file already exists"
        return 1
    fi
    
    cat > "$file" << 'EOF'
# Slick NAT Batch Configuration Template
# 
# Format:
#   add <interface> <internal_prefix/len> <external_prefix/len>
#   del <interface> <internal_prefix/len>
#   drop <interface>    - Drop all mappings for interface
#   drop --all         - Drop all mappings
#
# Examples:
#   add eth0 2001:db8:internal:1::/64 2001:db8:external:1::/64
#   add eth0 2001:db8:internal:2::/64 2001:db8:external:2::/64
#   del eth0 2001:db8:internal:3::/64
#   drop eth1
#   drop --all
#
# Notes:
#   - Lines starting with # are ignored
#   - Empty lines are ignored
#   - Both prefixes must have the same prefix length for add operations
#   - Interface must exist when the operation is processed
#   - Drop operations will remove all mappings for the interface

# Add your operations below:

EOF
    
    echo "Template created: $file"
    echo "Edit the file and use '$0 add-batch $file' to apply"
    return 0
}
