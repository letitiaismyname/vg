#include "gam_index.hpp"

#include <iostream>

namespace vg {

using namespace std;

auto GAMIndex::bins_of_id(id_t id) -> vector<bin_t> {
    vector<bin_t> to_return;
    
    // How many bits are in the number? We get a bin per bit.
    auto number_bits = CHAR_BIT * sizeof(bin_t);
    
    // We need to keep in mind that shifting *all* the bits out of a number in
    // one go is undefined behavior.
    
    // Bin number consists of an index which is a prefix of the number
    bin_t bin_index = ((bin_t)id) >> 1;
    // And an offset to keep from colliding with other bins.
    bin_t bin_offset = ((bin_t)~0) >> 1;
    
    for (int i = 0; i < number_bits; i++) {
        
#ifdef debug
        cerr << hex << id << " " << i << " " << bin_index << " " << bin_offset << " " << (bin_index + bin_offset) << endl;
#endif
    
        to_return.push_back(bin_index + bin_offset);
        bin_index = bin_index >> 1;
        bin_offset = bin_offset >> 1;
    }
    
    return to_return;
}

auto GAMIndex::bins_of_range(id_t min_id, id_t max_id) -> vector<bin_t> {
    // We can just get the two bin vectors for the ending bin, and generate an inclusive range of bins at each level.
    
    vector<bin_t> to_return;
    
    auto min_bins = bins_of_id(min_id);
    auto max_bins = bins_of_id(max_id);
    
    assert(min_bins.size() == max_bins.size());
    
    for (size_t i = 0; i < min_bins.size(); i++) {
        // For each specificity level
        for (bin_t bin = min_bins[i]; bin != max_bins[i] + 1; bin++) {
            // For each bin in the inclusive range, emit it
            to_return.push_back(bin);
        }
    }
    
    return to_return;
    
}
    
auto GAMIndex::common_bin(id_t a, id_t b) -> bin_t {
    // Convert to unsigned numbers
    bin_t a_bin = a;
    bin_t b_bin = b;
    
    // Define the offset for the bin
    bin_t offset = ((bin_t)~0);
    
    // We're just going to pop off bits until we find the common prefix.
    // Always pop off one bit, even if we are binning a number and itself.
    // TODO: Find a faster way to do this with the appropriate instruction intrinsics.
    do {
        a_bin = a_bin >> 1;
        b_bin = b_bin >> 1;
        offset = offset >> 1;
    } while(a_bin != b_bin);
    return a_bin + offset;
}

auto GAMIndex::window_of_id(id_t id) -> window_t  {
    return id >> WINDOW_SHIFT;
}

auto GAMIndex::add_group(id_t min_id, id_t max_id, int64_t virtual_start, int64_t virtual_past_end) -> void {
    // Find the bin for the run
    bin_t bin = common_bin(min_id, max_id);
    
#ifdef debug
    cerr << "Group spanning " << min_id << "-" << max_id << " at "
        << virtual_start << "-" << virtual_past_end << " lands in bin " << bin << endl;
#endif
    
    // Find the existing ranges in the bin.
    // We know the previous one, if present, must end at or before this one's start.
    auto& ranges = bin_to_ranges[bin];
    
    if (!ranges.empty() && ranges.back().second == virtual_start) {
        // We fit right after the last range.
        ranges.back().second = virtual_past_end;
#ifdef debug
        cerr << "Extend existing range to " << ranges.back().first << "-" << ranges.back().second << endl;
#endif
    } else {
        // We need a new range
        bin_to_ranges[bin].emplace_back(virtual_start, virtual_past_end);
    }
    
    for (window_t w = window_of_id(min_id); w <= window_of_id(max_id); w++) {
        // For each window that this group overlaps
        
        if (!window_to_start.count(w)) {
            // If it is the first group we encounter in the window, it must also be the earliest-staring group in the window.
            
            // This is the earliest virtual offset to overlap that window
            window_to_start[w] = virtual_start;
            
#ifdef debug
            cerr << "Start window " << w << endl;
#endif
        }
    }
}

auto GAMIndex::find(id_t node_id) const -> vector<pair<int64_t, int64_t>> {
    vector<pair<int64_t, int64_t>> to_return;
    
    find(node_id, [&](int64_t run_start, int64_t run_past_end) -> bool {
        // For each run we find, remember it
        to_return.emplace_back(run_start, run_past_end);
        // Keep getting runs until we run out in the index. We can't actually scan the data.
        return true;
    });
    
    return to_return;
}

auto GAMIndex::find(id_t node_id, const function<bool(int64_t, int64_t)> scan_callback) const -> void {
    // Look for a single-node inclusive range
    find(node_id, node_id, std::move(scan_callback));
}

auto GAMIndex::find(id_t min_node, id_t max_node, const function<bool(int64_t, int64_t)> scan_callback) const -> void {
    
#ifdef debug
    cerr << "Query for node range " << min_node << "-" << max_node << endl;
#endif
    
    // Find the window that gives us a lower bound on the virtual offset we
    // need to be at to find things that touch this node ID.
    window_t min_window = window_of_id(min_node);
    window_t max_window = window_of_id(max_node);
    
#ifdef debug
    cerr << "Looking for first filled window of " << min_window << "-" << max_window << endl;
#endif
    
    // Find the minimum virtual offset we need to consider
    int64_t min_vo;
    // It will be for the first occupied window at or after the min window but not greater than the max window.
    auto found = window_to_start.lower_bound(min_window);
    if (found != window_to_start.end() && found->first <= max_window) {
        // Some groups overlapped this window, and they started here.
        min_vo = found->second;
        
#ifdef debug
        cerr << "First occupied window is " << found->first << " at offset " << min_vo << endl;
#endif
    } else {
        // No groups overlapped any window within the range, so don't iterate anything.
        
#ifdef debug
        cerr << "No windows occupied; range is empty" << endl;
#endif
        
        return;
    }
    
    // Find the bins that any of the nodes in the range can be in
    auto bin_numbers = bins_of_range(min_node, max_node);
    
    // Filter down to bins that actually have vectors in the index
    vector<decltype(bin_to_ranges)::const_iterator> used_bins;
    for (auto& bin_number : bin_numbers) {
        auto found = bin_to_ranges.find(bin_number);
        
        if (found != bin_to_ranges.end()) {
            used_bins.push_back(found);
        }
    }
    
    // Set up a cursor in each bin
    // TODO: Could we do one cursor per specificity level instead? This way might be introducing some n^2 stuff in the range length.
    vector<vector<pair<int64_t, int64_t>>::const_iterator> cursors;
    for (auto& bin : used_bins) {
        cursors.push_back(bin->second.begin());
#ifdef debug
        cerr << "Bin " << bin->first << " overlaps the query and is nonempty" << endl;
#endif
    }
    
    while (true) {
        // Loop until the user asks us to stop or we run out of things to give them.
    
#ifdef debug
        cerr << "Find earliest-starting run in any bin ending after " << min_vo << endl;
#endif
    
        // This tracks which of the cursors points to the run that starts earliest, or the max value if no candidate runs exist.
        size_t starts_earliest = numeric_limits<size_t>::max();
        
        for(size_t i = 0; i < used_bins.size(); i++) {
            // Advance each cursor to the earliest-starting window that ends after the min_vo, by linear scan
            auto& bin_ranges = used_bins[i]->second;
            auto& cursor = cursors[i];
            
            while (cursor != bin_ranges.end() && cursor->second <= min_vo) {
                // This run ends too early, so keep advancing.
                ++cursor;
            }
            
            if (cursor != bin_ranges.end()) {
                // We actually have a candidate run
                if (starts_earliest == numeric_limits<size_t>::max() || cursor->first < cursors[starts_earliest]->first) {
                    // This candidate run starts earlier than the earliest candidate run from other bins.
                    
                    // Rememebr it.
                    starts_earliest = i;
                }
                
            }
        }
        
        if (starts_earliest == numeric_limits<size_t>::max()) {
            // We are all out of runs in any of the bins. We are done!
#ifdef debug
            cerr << "Out of runs in bins" << endl;
#endif
            return;
        }
        
#ifdef debug
        cerr << "Found run " << cursors[starts_earliest]->first << "-" << cursors[starts_earliest]->second
            << " from bin " << used_bins[starts_earliest]->first << endl;
#endif
        
        // Call the callback with the range max(min_vo, that run's start) to that run's end.
        bool keep_going = scan_callback(max(min_vo, cursors[starts_earliest]->first), cursors[starts_earliest]->second);
        
        if (!keep_going) {
            // The user is done with runs. They must have found a group that has an out-of-range minimum node ID.
            // We are done!
            return;
        }
        
        // Look for the next run continuing after here.
        min_vo = cursors[starts_earliest]->second;
    }
}

auto GAMIndex::add_group(const vector<Alignment>& alns, int64_t virtual_start, int64_t virtual_past_end) -> void {
    // Find the min and max ID visited by any of the alignments
    id_t min_id = numeric_limits<id_t>::max();
    id_t max_id = numeric_limits<id_t>::min();
    
    for (auto& aln : alns) {
        // For each alignment
        if (aln.path().mapping_size() == 0) {
            // The read is unmapped, so it belongs to node ID 0
            min_id = min(min_id, (id_t)0);
            max_id = max(max_id, (id_t)0);
        } else {
            for (auto& mapping : aln.path().mapping()) {
                // For each mapping in it, min/max in the ID
                auto id = mapping.position().node_id();
                min_id = min(min_id, id);
                max_id = max(max_id, id);
            }
        }
    }
    
    add_group(min_id, max_id, virtual_start, virtual_past_end);
}

auto GAMIndex::index(cursor_t& cursor) -> void {
    // Keep track of what group we are in 
    int64_t group_vo = cursor.tell_group();
    // And load all its alignments
    vector<Alignment> group;
    
    // We need to have seek support
    assert(group_vo != -1);
    
    while (cursor.has_next()) {
        // For each alignment
        
        // Work out what group it is in
        int64_t alignment_group_vo = cursor.tell_group();
        
        if (alignment_group_vo != group_vo) {
            // This is the start of a new group
            
            // Record the old group as being up to here
            add_group(group, group_vo, alignment_group_vo);
            
            // Set up for the new group
            group.clear();
            group_vo = alignment_group_vo;
        }
        
        // Add the alignment to the group
        group.push_back(*cursor);
        
        // Go look at the next alignment
        cursor.get_next();
    }
    
    if (!group.empty()) {
        // Record the final group. Use wherever the cursor landed at the end as its final virtual offset.
        add_group(group, group_vo, cursor.tell_raw());
    }
}

auto GAMIndex::find(cursor_t& cursor, id_t min_node, id_t max_node,
    const function<void(const Alignment&)> handle_result) const -> void {
    
    // We need seek support
    assert(cursor.tell_raw() != -1);
    
    find(min_node, max_node, [&](int64_t start_vo, int64_t past_end_vo) -> bool {
        // For each matching range in the index
        
        if (start_vo != cursor.tell_raw()) {
            // Seek to the start if we aren't there already, or if we are somehow in the middle of that group.
            cursor.seek_group(start_vo);
        }
        
        // We need to track each group we encounter, so we can tell when an entire group is past the top end of the ID range.
        int64_t group_vo = cursor.tell_group();
        id_t group_min_id = numeric_limits<id_t>::max();
        
        while (cursor.has_next() && cursor.tell_group() < past_end_vo) {
            // Read each alignment until we find a group that starts out of range
            
            // Which group is this alignment in?
            auto alignment_group_vo = cursor.tell_group();
            
            if (alignment_group_vo != group_vo) {
                // We finished the previous group.
                
                if (group_min_id != numeric_limits<id_t>::max() && group_min_id > max_node) {
                    // Everything in the (non-empty) previous group was too high. We don't care about this group; our iteration is over.
                    return false;
                }
                
                // Otherwise we need to start a new group
                group_vo = alignment_group_vo;
                group_min_id = numeric_limits<id_t>::max();
            }
            
            // Filter the alignment by the query and yield it if it matches
            const auto& alignment = *cursor;
            bool alignment_match = false;
            
            if (alignment.path().mapping_size() == 0) {
                // This read is unmapped, so count it as node 0.
                group_min_id = min(group_min_id, (id_t)0);
                if (0 >= min_node && 0 <= max_node) {
                    // We want unmapped reads.
                    alignment_match = true;
                }
            } else {
                // The read has mappings
                for (const auto& mapping : alignment.path().mapping()) {
                    // Look at each node that is visited
                    auto visited = mapping.position().node_id();
                    group_min_id = min(group_min_id, visited);
                    if (visited >= min_node && visited <= max_node) {
                        // We want this node.
                        alignment_match = true;
                        break;
                    }
                    
                }
            }
            
            if (alignment_match) {
                // This alignment is one that matches the query. Yield it.
                handle_result(alignment);
            }
            
        }
       
        if (group_min_id != numeric_limits<id_t>::max() && group_min_id > max_node) {
            // If the (non-empty) last group had all its node IDs past the max
            // node ID, we know nothing after it can possibly match, so stop
            // iteration.
            return false;
        }
        
        // Otherwise, the last group we looked at was not yet out of bounds, so get another range to look at, if one exists.
        return true;
    });
}

auto GAMIndex::find(cursor_t& cursor, id_t node_id, const function<void(const Alignment&)> handle_result) const -> void {
    find(cursor, node_id, node_id, std::move(handle_result));
}

auto GAMIndex::save(ostream& to) const -> void {
    // We aren't going to save as Protobuf messages; we're going to save as a bunch of varints.
    
    // Format is
    // Bin count (varint64)
    // For each bin:
    // Bin number (varint64)
    // Run count (varint64)
    // For each run:
    // Start (varint64)
    // Past-end (varint64)
    // And then window count (varint64)
    // And for each window:
    // Window number (varint64)
    // Window start (varint64)
    
    // All the integers are Protobuf variable-length values.
    // The result is gzip-compressed.
    
    ::google::protobuf::io::OstreamOutputStream raw_out(&to);
    ::google::protobuf::io::GzipOutputStream gzip_out(&raw_out);
    ::google::protobuf::io::CodedOutputStream coded_out(&gzip_out);
    
    // Save the bin count
    coded_out.WriteVarint64(bin_to_ranges.size());
    for (auto& kv : bin_to_ranges) {
        // For each bin, save the number
        coded_out.WriteVarint64(kv.first);
        // And the number of runs
        coded_out.WriteVarint64(kv.second.size());
        
        for (auto& run : kv.second) {
            // For each run, write the VO range
            coded_out.WriteVarint64(run.first);
            coded_out.WriteVarint64(run.second);
        }
    }
    
    // Save the window count
    coded_out.WriteVarint64(window_to_start.size());
    for (auto& kv : window_to_start) {
        // Save each window's number and start
        coded_out.WriteVarint64(kv.first);
        coded_out.WriteVarint64(kv.second);
    }
    
}

auto GAMIndex::load(istream& from) -> void {

    ::google::protobuf::io::IstreamInputStream raw_in(&from);
    ::google::protobuf::io::GzipInputStream gzip_in(&raw_in);
    

    bin_to_ranges.clear();
    window_to_start.clear();
    
    // Define an error handling function
    auto handle = [](bool ok) {
        if (!ok) throw std::runtime_error("GAMIndex::load detected corrupt index file");
    };
    
    // Read the number of bins that are used
    uint64_t bin_count;
    {
        // TODO: To avoid hitting the coded input stream's byte limit (why is
        // it even at this level?) we destory and recreate it for every
        // semantic group.
        ::google::protobuf::io::CodedInputStream coded_in(&gzip_in);
        handle(coded_in.ReadVarint64(&bin_count));
    }
    
    for (size_t i = 0; i < bin_count; i++) {
        // Read the bin number and run count for each bin
        uint64_t bin_number;
        uint64_t run_count;
        {
            ::google::protobuf::io::CodedInputStream coded_in(&gzip_in);
            handle(coded_in.ReadVarint64(&bin_number));
            handle(coded_in.ReadVarint64(&run_count));
        }
        
        // Create the empty bin
        auto& runs = bin_to_ranges[bin_number];
        
        for (size_t j = 0; j < run_count; j++) {
            // Load each run
            uint64_t run_start;
            uint64_t run_end;
            
            {
                ::google::protobuf::io::CodedInputStream coded_in(&gzip_in);
                handle(coded_in.ReadVarint64(&run_start));
                handle(coded_in.ReadVarint64(&run_end));
            }
            
            runs.emplace_back(run_start, run_end);
            
        }
        
    }
    
    
    // Now count the number of windows
    uint64_t window_count;
    {
        ::google::protobuf::io::CodedInputStream coded_in(&gzip_in);
        handle(coded_in.ReadVarint64(&window_count));
    }
    
    for (size_t i = 0; i < window_count; i++) {
        // Load each window
        uint64_t window_number;
        uint64_t window_start;
        
        {
            ::google::protobuf::io::CodedInputStream coded_in(&gzip_in);
            handle(coded_in.ReadVarint64(&window_number));
            handle(coded_in.ReadVarint64(&window_start));
        }
        
        window_to_start[window_number] = window_start;
    }

}

}
