COREPATH =../..

plug_in_name = stats
QUERY_STATS_NAME = sems-stats
QUERY_STATS_DEST_NAME = $(APP_NAME)-stats

module_ldflags = 
module_cflags  = 

extra_target = $(QUERY_STATS_NAME)
extra_install = install_query_stats
extra_clean = clean_query_stats

include ../Makefile.app_module

%.o: %.cxx $(COREPATH)/../Makefile.defs
	$(CXX) -MMD $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@

$(QUERY_STATS_NAME): query_stats.o Makefile
	$(LD) -o $(QUERY_STATS_NAME) query_stats.o $(LDFLAGS) $(EXTRA_LDFLAGS) 

install_query_stats: $(QUERY_STATS_NAME) $(DESTDIR)$(bin_prefix)/$(bin_dir)
	$(INSTALL_BIN) $(QUERY_STATS_NAME) $(DESTDIR)$(bin_prefix)/$(bin_dir)/$(QUERY_STATS_DEST_NAME)

clean_query_stats: 
	rm -f $(QUERY_STATS_NAME) query_stats.o query_stats.d
